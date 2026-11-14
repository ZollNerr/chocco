#!/usr/bin/env python3
"""
Примеры интеграции с Light Watcher v2.0 (только стандартная библиотека Python 3).

  1) Опрос состояния:     python3 lightwatcher_client.py status http://lightwatcher.local
  2) Слежение (polling):  python3 lightwatcher_client.py watch  http://192.168.1.50 10
  3) Приём webhook:       python3 lightwatcher_client.py serve  8080
       затем в боте: /webhook http://<IP_этого_компьютера>:8080/hook

В обработчиках on_power_off / on_power_on пишите свою логику:
CRM, SMS, запуск генератора, запись в базу, Slack и т.д.
"""
import json
import sys
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer


def on_power_off(info: dict) -> None:
    print(f"[{info.get('device')}] СВЕТ ПРОПАЛ в {info.get('time') or info.get('outage_started')}")


def on_power_on(info: dict) -> None:
    dur = info.get("outage_duration_s") or info.get("last_outage_duration_s") or 0
    print(f"[{info.get('device')}] СВЕТ ЕСТЬ, отключение длилось {dur // 60} мин")


def get_status(base: str) -> dict:
    with urllib.request.urlopen(base.rstrip("/") + "/api/status", timeout=5) as r:
        return json.load(r)


def watch(base: str, interval: int) -> None:
    last = None
    while True:
        try:
            s = get_status(base)
            if last is not None and s["power"] != last:
                (on_power_on if s["power"] else on_power_off)(s)
            last = s["power"]
        except Exception as e:  # устройство недоступно — тоже сигнал (нет света и батарея села?)
            print("нет связи с устройством:", e)
        time.sleep(interval)


class Hook(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        try:
            ev = json.loads(body)
        except ValueError:
            self.send_response(400); self.end_headers(); return
        if ev.get("event") == "power_off":
            on_power_off(ev)
        elif ev.get("event") == "power_on":
            on_power_on(ev)
        else:
            print("событие:", ev)
        self.send_response(200); self.end_headers(); self.wfile.write(b"ok")

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "status":
        print(json.dumps(get_status(sys.argv[2]), ensure_ascii=False, indent=2))
    elif cmd == "watch":
        watch(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 10)
    elif cmd == "serve":
        print(f"Жду webhook на :{sys.argv[2]}/hook")
        HTTPServer(("0.0.0.0", int(sys.argv[2])), Hook).serve_forever()
