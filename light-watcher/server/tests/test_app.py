import os
import sys
import time

import pytest

os.environ["LW_NO_AUTOAPP"] = "1"
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from fastapi.testclient import TestClient  # noqa: E402

import app as hub  # noqa: E402

KEY, ADMIN = "ingest-secret", "admin-secret"
A = {"Authorization": f"Bearer {ADMIN}"}


@pytest.fixture
def ctx(tmp_path):
    s = hub.Settings(ingest_key=KEY, admin_key=ADMIN, db_path=str(tmp_path / "t.db"), watchdog=False)
    application = hub.create_app(s)
    with TestClient(application) as c:
        yield c, application.state.store, s


def post(c, payload, key=KEY):
    return c.post(f"/api/v1/ingest?key={key}", json=payload)


def ev(event, ts, **kw):
    return {"event": event, "device_id": "abc123", "device": "Дача", "ts": ts, **kw}


def test_auth(ctx):
    c, _, _ = ctx
    assert post(c, ev("power_off", int(time.time())), key="wrong").status_code == 401
    assert c.post("/api/v1/ingest", json=ev("boot", 0), headers={"X-LightWatcher-Key": KEY}).status_code == 200
    assert post(c, {"event": "power_off"}).status_code == 422
    assert post(c, {"event": "hack", "device_id": "x"}).status_code == 422
    assert c.patch("/api/v1/sites/abc123", json={"title": "x"}).status_code == 401


def test_outage_cycle_and_dedup(ctx):
    c, _, _ = ctx
    now = int(time.time())
    assert post(c, ev("power_off", now - 3600)).json()["duplicate"] is False
    assert post(c, ev("power_off", now - 3600)).json()["duplicate"] is True  # повтор устройства
    s = c.get("/api/v1/sites/abc123").json()
    assert s["state"] == "off" and s["since"] == now - 3600
    post(c, ev("power_on", now - 600, outage_duration_s=3000))
    s = c.get("/api/v1/sites/abc123").json()
    assert s["state"] == "on"
    assert s["outages"][0]["duration_s"] == 3000
    assert 2990 <= s["downtime_24h_s"] <= 3010


def test_lost_off_event_restored_from_duration(ctx):
    c, _, _ = ctx
    now = int(time.time())
    post(c, ev("boot", now - 7200, power=True))
    post(c, ev("power_on", now - 100, outage_duration_s=1800))  # power_off потерялся
    o = c.get("/api/v1/sites/abc123/outages").json()
    assert len(o) == 1 and o[0]["off"] == now - 1900 and o[0]["duration_s"] == 1800


def test_heartbeat_reconciles_and_offline(ctx):
    c, store, s = ctx
    now = int(time.time())
    post(c, ev("heartbeat", 0, power=False, outage_started=now - 500, interval_s=600, battery_pct=55))
    site = c.get("/api/v1/sites/abc123", headers=A).json()
    assert site["state"] == "off" and site["since"] == now - 500 and site["battery_pct"] == 55
    assert site["hb_interval"] == 600 and site["online"]
    # через 40 минут без пульса устройство считается пропавшим (3 x 10 мин + 1 мин)
    assert hub.watchdog_tick(store, s, now + 2400) == ["abc123"]
    assert hub.watchdog_tick(store, s, now + 2500) == []  # алерт только один раз
    assert post(c, ev("heartbeat", 0, power=True, power_since=now, interval_s=600)).json()["back_online"] is True


def test_admin_patch_private_and_list(ctx):
    c, _, _ = ctx
    post(c, ev("power_off", int(time.time())))
    r = c.patch("/api/v1/sites/abc123", json={"title": "Склад №2", "lat": 50.45, "lon": 30.52, "public": False}, headers=A)
    assert r.status_code == 200 and r.json()["name"] == "Склад №2"
    assert c.get("/api/v1/sites").json() == []                      # приватный — не виден публично
    assert c.get("/api/v1/sites/abc123").status_code == 404
    assert c.get("/api/v1/sites", headers=A).json()[0]["lat"] == 50.45
    assert c.patch("/api/v1/sites/abc123", json={"lat": 200}, headers=A).status_code == 422


def test_report_and_csv(ctx):
    c, _, _ = ctx
    now = int(time.time())
    post(c, ev("power_off", now - 5 * 3600))
    post(c, ev("power_on", now - 3 * 3600, outage_duration_s=7200))
    rep = c.get("/api/v1/sites/abc123/report?days=7").json()
    assert rep["downtime_s"] == 7200 and rep["outages"] == 1
    assert len(rep["days"]) == 7 and 0 < rep["availability_pct"] < 100
    csv_text = c.get("/api/v1/sites/abc123/outages.csv").text
    assert csv_text.splitlines()[0] == "site,off,on,duration_min" and ",120.0" in csv_text


def test_metrics_and_index(ctx):
    c, _, _ = ctx
    post(c, ev("power_off", int(time.time())))
    m = c.get("/metrics").text
    assert 'lightwatcher_hub_power{site="abc123",name="Дача"} 0' in m
    assert "Light Watcher Hub" in c.get("/").text
    assert c.delete("/api/v1/sites/abc123", headers=A).json() == {"ok": True}
    assert c.get("/api/v1/sites").json() == []
