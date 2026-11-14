"""
Light Watcher Hub — сервер-агрегатор для многих устройств Light Watcher.

Устройства шлют события на webhook (`/webhook https://hub.example.com/api/v1/ingest?key=...`),
сервер хранит историю в SQLite и показывает карту/таблицу объектов, отчёты доступности (SLA),
CSV-выгрузку, Prometheus-метрики и (опционально) шлёт в Telegram алерт,
если устройство перестало выходить на связь.

Переменные окружения:
  LW_INGEST_KEY      ключ для приёма событий от устройств (обязательно)
  LW_ADMIN_KEY       ключ администратора (редактирование, приватные объекты)
  LW_DB              путь к SQLite (по умолчанию ./lightwatcher.db)
  LW_TZ              часовой пояс для суточных отчётов (Europe/Kyiv)
  LW_PUBLIC          1 — публичные объекты видны всем без ключа (по умолчанию 1)
  LW_OFFLINE_FACTOR  устройство "пропало", если нет пульса дольше N интервалов (3)
  LW_TG_TOKEN, LW_TG_CHAT   Telegram для алертов "устройство не на связи" (необязательно)
  LW_WATCHDOG        0 — отключить фоновую проверку (для тестов)
"""
from __future__ import annotations

import csv
import hmac
import io
import json
import os
import sqlite3
import threading
import time
import urllib.parse
import urllib.request
from contextlib import asynccontextmanager, contextmanager
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from datetime import time as dtime
from pathlib import Path
from typing import Optional
from zoneinfo import ZoneInfo

from fastapi import Depends, FastAPI, HTTPException, Query, Request
from fastapi.responses import HTMLResponse, PlainTextResponse, Response
from pydantic import BaseModel, Field

EVENTS = {"power_off", "power_on", "boot", "test", "battery_low", "heartbeat"}
MIN_VALID_TS = 1_600_000_000  # всё, что раньше, — устройство без NTP-времени


@dataclass
class Settings:
    ingest_key: str = field(default_factory=lambda: os.environ.get("LW_INGEST_KEY", ""))
    admin_key: str = field(default_factory=lambda: os.environ.get("LW_ADMIN_KEY", ""))
    db_path: str = field(default_factory=lambda: os.environ.get("LW_DB", "lightwatcher.db"))
    tz: str = field(default_factory=lambda: os.environ.get("LW_TZ", "Europe/Kyiv"))
    public: bool = field(default_factory=lambda: os.environ.get("LW_PUBLIC", "1") == "1")
    offline_factor: float = field(default_factory=lambda: float(os.environ.get("LW_OFFLINE_FACTOR", "3")))
    tg_token: str = field(default_factory=lambda: os.environ.get("LW_TG_TOKEN", ""))
    tg_chat: str = field(default_factory=lambda: os.environ.get("LW_TG_CHAT", ""))
    watchdog: bool = field(default_factory=lambda: os.environ.get("LW_WATCHDOG", "1") == "1")


SCHEMA = """
CREATE TABLE IF NOT EXISTS sites(
  id TEXT PRIMARY KEY,
  device_name TEXT, title TEXT, address TEXT, lat REAL, lon REAL,
  public INTEGER NOT NULL DEFAULT 1,
  power INTEGER, state_since INTEGER, last_seen INTEGER,
  hb_interval INTEGER NOT NULL DEFAULT 0,
  battery_pct INTEGER, rssi INTEGER, ip TEXT, version TEXT, outages_total INTEGER,
  offline_alerted INTEGER NOT NULL DEFAULT 0,
  created INTEGER
);
CREATE TABLE IF NOT EXISTS outages(
  id INTEGER PRIMARY KEY, site_id TEXT NOT NULL, off_ts INTEGER NOT NULL, on_ts INTEGER
);
CREATE INDEX IF NOT EXISTS outages_site ON outages(site_id, off_ts);
CREATE TABLE IF NOT EXISTS events(
  id INTEGER PRIMARY KEY, site_id TEXT NOT NULL, event TEXT NOT NULL,
  ts INTEGER NOT NULL, received INTEGER NOT NULL, payload TEXT
);
CREATE UNIQUE INDEX IF NOT EXISTS events_dedup ON events(site_id, event, ts);
"""


def _int(v, default: int = 0) -> int:
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


class Store:
    def __init__(self, settings: Settings):
        self.s = settings
        self.tz = ZoneInfo(settings.tz)
        self.lock = threading.Lock()
        with self.conn() as c:
            c.executescript(SCHEMA)

    @contextmanager
    def conn(self):
        c = sqlite3.connect(self.s.db_path, timeout=10)
        c.row_factory = sqlite3.Row
        c.execute("PRAGMA journal_mode=WAL")
        try:
            yield c
            c.commit()
        finally:
            c.close()

    # ------------------------------------------------------------------ запись
    def ingest(self, p: dict, now: int) -> dict:
        dev = str(p.get("device_id", "")).strip()
        if not dev or len(dev) > 32 or not dev.replace("-", "").replace("_", "").isalnum():
            raise ValueError("device_id")
        ev = str(p.get("event", ""))
        if ev not in EVENTS:
            raise ValueError("event")
        ts = _int(p.get("ts"))
        if ts < MIN_VALID_TS or ts > now + 300:
            ts = now

        with self.lock, self.conn() as c:
            site = c.execute("SELECT * FROM sites WHERE id=?", (dev,)).fetchone()
            if site is None:
                c.execute("INSERT INTO sites(id, device_name, created) VALUES(?,?,?)",
                          (dev, str(p.get("device") or dev)[:64], now))
                site = c.execute("SELECT * FROM sites WHERE id=?", (dev,)).fetchone()

            if ev != "heartbeat":  # пульс не храним в журнале — только обновляем объект
                try:
                    c.execute("INSERT INTO events(site_id,event,ts,received,payload) VALUES(?,?,?,?,?)",
                              (dev, ev, ts, now, json.dumps(p, ensure_ascii=False)[:4000]))
                except sqlite3.IntegrityError:
                    return {"ok": True, "duplicate": True, "back_online": False}

            upd = {"last_seen": now, "offline_alerted": 0,
                   "device_name": str(p.get("device") or site["device_name"])[:64]}
            for k in ("battery_pct", "rssi", "ip", "version", "outages_total"):
                if k in p and p[k] is not None:
                    upd[k] = p[k]
            if ev == "heartbeat":
                upd["hb_interval"] = _int(p.get("interval_s"))

            if ev == "power_off":
                self._set_power(c, dev, False, ts)
            elif ev == "power_on":
                dur = _int(p.get("outage_duration_s"))
                self._set_power(c, dev, True, ts, off_hint=ts - dur if dur > 0 else None)
            elif ev == "battery_low":
                self._reconcile(c, dev, False, ts)
            elif ev in ("heartbeat", "boot") and "power" in p:
                power = bool(p["power"])
                since = _int(p.get("power_since") if power else p.get("outage_started"))
                self._reconcile(c, dev, power, since if MIN_VALID_TS <= since <= now else now)

            cols = ", ".join(f"{k}=?" for k in upd)
            c.execute(f"UPDATE sites SET {cols} WHERE id=?", (*upd.values(), dev))
            return {"ok": True, "duplicate": False, "back_online": bool(site["offline_alerted"])}

    def _set_power(self, c, dev: str, on: bool, ts: int, off_hint: Optional[int] = None):
        open_ = c.execute("SELECT id, off_ts FROM outages WHERE site_id=? AND on_ts IS NULL "
                          "ORDER BY off_ts DESC LIMIT 1", (dev,)).fetchone()
        if not on:
            if open_ is None:
                c.execute("INSERT INTO outages(site_id, off_ts) VALUES(?,?)", (dev, ts))
            since = open_["off_ts"] if open_ else ts
            c.execute("UPDATE sites SET power=0, state_since=? WHERE id=?", (since, dev))
            return
        if open_ is not None:
            c.execute("UPDATE outages SET on_ts=? WHERE id=?", (max(ts, open_["off_ts"]), open_["id"]))
        elif off_hint and off_hint < ts:
            # событие "свет пропал" потерялось — восстанавливаем отключение по длительности
            dup = c.execute("SELECT 1 FROM outages WHERE site_id=? AND ABS(off_ts-?)<120", (dev, off_hint)).fetchone()
            if not dup:
                c.execute("INSERT INTO outages(site_id, off_ts, on_ts) VALUES(?,?,?)", (dev, off_hint, ts))
        c.execute("UPDATE sites SET power=1, state_since=? WHERE id=?", (ts, dev))

    def _reconcile(self, c, dev: str, power: bool, since: int):
        row = c.execute("SELECT power FROM sites WHERE id=?", (dev,)).fetchone()
        if row["power"] is None or bool(row["power"]) != power:
            self._set_power(c, dev, power, since)

    def update_site(self, dev: str, fields: dict) -> bool:
        if not fields:
            return self.exists(dev)
        with self.lock, self.conn() as c:
            cols = ", ".join(f"{k}=?" for k in fields)
            return c.execute(f"UPDATE sites SET {cols} WHERE id=?", (*fields.values(), dev)).rowcount > 0

    def delete_site(self, dev: str) -> bool:
        with self.lock, self.conn() as c:
            c.execute("DELETE FROM outages WHERE site_id=?", (dev,))
            c.execute("DELETE FROM events WHERE site_id=?", (dev,))
            return c.execute("DELETE FROM sites WHERE id=?", (dev,)).rowcount > 0

    def mark_alerted(self, dev: str):
        with self.lock, self.conn() as c:
            c.execute("UPDATE sites SET offline_alerted=1 WHERE id=?", (dev,))

    # ------------------------------------------------------------------ чтение
    def exists(self, dev: str) -> bool:
        with self.conn() as c:
            return c.execute("SELECT 1 FROM sites WHERE id=?", (dev,)).fetchone() is not None

    def _downtime(self, c, dev: str, start: int, end: int) -> int:
        rows = c.execute("SELECT off_ts, on_ts FROM outages WHERE site_id=? AND off_ts < ? "
                         "AND (on_ts IS NULL OR on_ts > ?)", (dev, end, start)).fetchall()
        return sum(max(0, min(r["on_ts"] or end, end) - max(r["off_ts"], start)) for r in rows)

    def is_offline(self, row, now: int) -> bool:
        hb = row["hb_interval"] or 0
        return hb > 0 and row["last_seen"] is not None and now - row["last_seen"] > hb * self.s.offline_factor + 60

    def _view(self, c, row, now: int, admin: bool) -> dict:
        offline = self.is_offline(row, now)
        if row["power"] is None:
            state = "unknown"
        else:
            state = "on" if row["power"] else "off"
        since = row["state_since"]
        v = {
            "id": row["id"],
            "name": row["title"] or row["device_name"] or row["id"],
            "address": row["address"],
            "lat": row["lat"], "lon": row["lon"],
            "state": state,
            "power": None if row["power"] is None else bool(row["power"]),
            "since": since,
            "duration_s": now - since if since else None,
            "online": not offline,
            "last_seen": row["last_seen"],
            "battery_pct": row["battery_pct"],
            "version": row["version"],
            "downtime_24h_s": self._downtime(c, row["id"], now - 86400, now),
            "downtime_7d_s": self._downtime(c, row["id"], now - 7 * 86400, now),
        }
        if admin:
            v.update({"public": bool(row["public"]), "ip": row["ip"], "rssi": row["rssi"],
                      "device_name": row["device_name"], "hb_interval": row["hb_interval"],
                      "outages_total": row["outages_total"]})
        return v

    def sites(self, now: int, admin: bool) -> list[dict]:
        with self.conn() as c:
            q = "SELECT * FROM sites" + ("" if admin else " WHERE public=1") + " ORDER BY COALESCE(title, device_name, id)"
            return [self._view(c, r, now, admin) for r in c.execute(q).fetchall()]

    def site(self, dev: str, now: int, admin: bool) -> Optional[dict]:
        with self.conn() as c:
            r = c.execute("SELECT * FROM sites WHERE id=?", (dev,)).fetchone()
            if r is None or (not admin and not r["public"]):
                return None
            return self._view(c, r, now, admin)

    def outages(self, dev: str, limit: int = 50, since: int = 0) -> list[dict]:
        with self.conn() as c:
            rows = c.execute("SELECT off_ts, on_ts FROM outages WHERE site_id=? AND off_ts >= ? "
                             "ORDER BY off_ts DESC LIMIT ?", (dev, since, limit)).fetchall()
        return [{"off": r["off_ts"], "on": r["on_ts"],
                 "duration_s": (r["on_ts"] - r["off_ts"]) if r["on_ts"] else None} for r in rows]

    def report(self, dev: str, days: int, now: int) -> dict:
        today = datetime.fromtimestamp(now, self.tz).date()
        out, total_dt, total_span, total_cnt = [], 0, 0, 0
        with self.conn() as c:
            for i in range(days - 1, -1, -1):
                d = today - timedelta(days=i)
                start = int(datetime.combine(d, dtime.min, self.tz).timestamp())
                end = min(int(datetime.combine(d + timedelta(days=1), dtime.min, self.tz).timestamp()), now)
                if end <= start:
                    continue
                dt = self._downtime(c, dev, start, end)
                cnt = c.execute("SELECT COUNT(*) FROM outages WHERE site_id=? AND off_ts>=? AND off_ts<?",
                                (dev, start, end)).fetchone()[0]
                out.append({"date": d.isoformat(), "downtime_s": dt, "outages": cnt,
                            "availability_pct": round(100 * (1 - dt / (end - start)), 2)})
                total_dt, total_span, total_cnt = total_dt + dt, total_span + end - start, total_cnt + cnt
        return {"days": out, "downtime_s": total_dt, "outages": total_cnt,
                "availability_pct": round(100 * (1 - total_dt / total_span), 2) if total_span else None}

    def newly_offline(self, now: int) -> list[sqlite3.Row]:
        with self.conn() as c:
            rows = c.execute("SELECT * FROM sites WHERE hb_interval>0 AND offline_alerted=0").fetchall()
        return [r for r in rows if self.is_offline(r, now)]


# ---------------------------------------------------------------------- Telegram
def tg_send(settings: Settings, text: str) -> None:
    if not settings.tg_token or not settings.tg_chat:
        return

    def _send():
        data = urllib.parse.urlencode({"chat_id": settings.tg_chat, "text": text, "parse_mode": "HTML"}).encode()
        try:
            urllib.request.urlopen(f"https://api.telegram.org/bot{settings.tg_token}/sendMessage", data, timeout=10)
        except Exception as e:  # noqa: BLE001 — алерт не должен ронять сервер
            print("telegram error:", e)

    threading.Thread(target=_send, daemon=True).start()


def fmt_dur(s: int) -> str:
    h, m = divmod(max(0, s) // 60, 60)
    return f"{h} год {m} хв" if h else f"{m} хв"


def watchdog_tick(store: Store, settings: Settings, now: int) -> list[str]:
    """Находит устройства, которые перестали слать пульс, и шлёт алерт (один раз)."""
    alerted = []
    for r in store.newly_offline(now):
        name = r["title"] or r["device_name"] or r["id"]
        last = "світло було" if r["power"] else "світла не було"
        tg_send(settings, f"⚠️ <b>{name}</b>: пристрій не виходить на зв'язок {fmt_dur(now - r['last_seen'])} "
                          f"(востаннє {last}). Можливо, розрядився акумулятор або немає інтернету.")
        store.mark_alerted(r["id"])
        alerted.append(r["id"])
    return alerted


# ---------------------------------------------------------------------- API
class SitePatch(BaseModel):
    title: Optional[str] = Field(None, max_length=64)
    address: Optional[str] = Field(None, max_length=200)
    lat: Optional[float] = Field(None, ge=-90, le=90)
    lon: Optional[float] = Field(None, ge=-180, le=180)
    public: Optional[bool] = None


def create_app(settings: Optional[Settings] = None) -> FastAPI:
    settings = settings or Settings()
    store = Store(settings)
    stop = threading.Event()

    def watchdog_loop():
        while not stop.wait(60):
            try:
                watchdog_tick(store, settings, int(time.time()))
            except Exception as e:  # noqa: BLE001
                print("watchdog error:", e)

    @asynccontextmanager
    async def lifespan(_app):
        t = None
        if settings.watchdog:
            t = threading.Thread(target=watchdog_loop, daemon=True)
            t.start()
        yield
        stop.set()

    app = FastAPI(title="Light Watcher Hub", version="1.0", lifespan=lifespan)
    app.state.store = store
    app.state.settings = settings

    def is_admin(request: Request) -> bool:
        if not settings.admin_key:
            return False
        auth = request.headers.get("authorization", "")
        key = auth[7:] if auth.lower().startswith("bearer ") else request.headers.get("x-admin-key", "")
        return bool(key) and hmac.compare_digest(key, settings.admin_key)

    def require_admin(request: Request) -> None:
        if not is_admin(request):
            raise HTTPException(401, "admin key required")

    def viewer(request: Request) -> bool:
        admin = is_admin(request)
        if not admin and not settings.public:
            raise HTTPException(401, "admin key required")
        return admin

    @app.post("/api/v1/ingest")
    async def ingest(request: Request, key: str = Query("")):
        if not settings.ingest_key:
            raise HTTPException(503, "LW_INGEST_KEY is not configured")
        key = key or request.headers.get("x-lightwatcher-key", "")
        if not key or not hmac.compare_digest(key, settings.ingest_key):
            raise HTTPException(401, "bad key")
        try:
            payload = await request.json()
        except ValueError:
            raise HTTPException(400, "invalid json")
        if not isinstance(payload, dict):
            raise HTTPException(400, "json object expected")
        try:
            res = store.ingest(payload, int(time.time()))
        except ValueError as e:
            raise HTTPException(422, f"invalid {e}")
        if res["back_online"]:
            s = store.site(payload["device_id"], int(time.time()), True)
            tg_send(settings, f"✅ <b>{s['name']}</b>: пристрій знову на зв'язку")
        return res

    @app.get("/api/v1/sites")
    def list_sites(admin: bool = Depends(viewer)):
        return store.sites(int(time.time()), admin)

    def get_visible(dev: str, admin: bool) -> dict:
        s = store.site(dev, int(time.time()), admin)
        if s is None:
            raise HTTPException(404, "site not found")
        return s

    @app.get("/api/v1/sites/{dev}")
    def get_site(dev: str, admin: bool = Depends(viewer)):
        s = get_visible(dev, admin)
        s["outages"] = store.outages(dev, 20)
        return s

    @app.get("/api/v1/sites/{dev}/outages")
    def get_outages(dev: str, limit: int = Query(50, ge=1, le=1000), admin: bool = Depends(viewer)):
        get_visible(dev, admin)
        return store.outages(dev, limit)

    @app.get("/api/v1/sites/{dev}/report")
    def get_report(dev: str, days: int = Query(30, ge=1, le=366), admin: bool = Depends(viewer)):
        s = get_visible(dev, admin)
        return {"site": s["id"], "name": s["name"], "tz": settings.tz, **store.report(dev, days, int(time.time()))}

    @app.get("/api/v1/sites/{dev}/outages.csv")
    def get_csv(dev: str, days: int = Query(90, ge=1, le=3660), admin: bool = Depends(viewer)):
        s = get_visible(dev, admin)
        tz = ZoneInfo(settings.tz)
        buf = io.StringIO()
        w = csv.writer(buf)
        w.writerow(["site", "off", "on", "duration_min"])
        for o in store.outages(dev, 100000, int(time.time()) - days * 86400):
            f = lambda t: datetime.fromtimestamp(t, tz).strftime("%Y-%m-%d %H:%M:%S") if t else ""  # noqa: E731
            w.writerow([s["name"], f(o["off"]), f(o["on"]), round(o["duration_s"] / 60, 1) if o["duration_s"] else ""])
        return Response(buf.getvalue(), media_type="text/csv; charset=utf-8",
                        headers={"Content-Disposition": f'attachment; filename="outages-{dev}.csv"'})

    @app.patch("/api/v1/sites/{dev}", dependencies=[Depends(require_admin)])
    def patch_site(dev: str, body: SitePatch):
        fields = body.model_dump(exclude_unset=True)
        if "public" in fields:
            fields["public"] = int(bool(fields["public"]))
        if not store.update_site(dev, fields):
            raise HTTPException(404, "site not found")
        return store.site(dev, int(time.time()), True)

    @app.delete("/api/v1/sites/{dev}", dependencies=[Depends(require_admin)])
    def delete_site(dev: str):
        if not store.delete_site(dev):
            raise HTTPException(404, "site not found")
        return {"ok": True}

    @app.get("/metrics", response_class=PlainTextResponse)
    def metrics(admin: bool = Depends(viewer)):
        lines = ["# TYPE lightwatcher_hub_power gauge", "# TYPE lightwatcher_hub_online gauge",
                 "# TYPE lightwatcher_hub_downtime_24h_seconds gauge"]
        for s in store.sites(int(time.time()), admin):
            name = str(s["name"]).replace("\\", "").replace('"', "'")
            lbl = f'{{site="{s["id"]}",name="{name}"}}'
            if s["power"] is not None:
                lines.append(f"lightwatcher_hub_power{lbl} {int(s['power'])}")
            lines.append(f"lightwatcher_hub_online{lbl} {int(s['online'])}")
            lines.append(f"lightwatcher_hub_downtime_24h_seconds{lbl} {s['downtime_24h_s']}")
        return "\n".join(lines) + "\n"

    @app.get("/healthz")
    def healthz():
        return {"ok": True}

    index_html = (Path(__file__).parent / "static" / "index.html").read_text(encoding="utf-8")

    @app.get("/", response_class=HTMLResponse)
    def index():
        return index_html

    return app


app = create_app() if os.environ.get("LW_NO_AUTOAPP") != "1" else None
