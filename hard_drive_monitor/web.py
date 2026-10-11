"""Minimal local dashboard: `hdmon serve` (stdlib http.server, 127.0.0.1 only)."""
from __future__ import annotations

import html
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, List

from .collect import collect
from .history import History
from .paths import default_db

BIND = "127.0.0.1"   # never exposed to the network
COLOR = {"OK": "#2e9e44", "WARNING": "#d49a00", "CRITICAL": "#d32f2f", "UNKNOWN": "#888"}


def svg_sparkline(values: List[Any], w: int = 160, h: int = 28) -> str:
    vals = [v for v in values if v is not None]
    if len(vals) < 2:
        return ""
    step = w / (len(vals) - 1)
    pts = " ".join(f"{i * step:.1f},{h - (v / 100) * (h - 2) - 1:.1f}" for i, v in enumerate(vals))
    return (f'<svg width="{w}" height="{h}" viewBox="0 0 {w} {h}"><polyline fill="none" stroke="#4a90d9" '
            f'stroke-width="2" points="{pts}"/></svg>')


def render_page(results, history: Dict[str, List[Dict[str, Any]]], meta: Dict[str, Any]) -> str:
    rows = []
    for d, a in results:
        hv = [r["health"] for r in history.get(d.key, [])]
        reasons = "<br>".join(html.escape(r) for r in a.reasons) or "&mdash;"
        vol = f" ({html.escape(' '.join(d.volumes))})" if d.volumes else ""
        rows.append(
            f"<tr><td><b>{html.escape(d.device)}</b>{vol}</td><td>{html.escape(d.model or '-')}</td>"
            f"<td>{html.escape(d.serial or '-')}</td><td>{d.kind}</td>"
            f"<td>{d.temperature_c if d.temperature_c is not None else '-'} &deg;C</td>"
            f"<td>{(d.power_on_hours or 0):,} h</td>"
            f"<td>{a.health if a.health is not None else 'n/a'}%</td><td>{a.performance if a.performance is not None else 'n/a'}%</td>"
            f"<td style='color:#fff;background:{COLOR[a.status]};font-weight:bold;text-align:center'>{a.status}</td>"
            f"<td>{svg_sparkline(hv)}</td><td class=r>{reasons}</td></tr>")
    return f"""<!doctype html><html><head><meta charset="utf-8"><title>Hard Drive Monitor</title>
<meta http-equiv="refresh" content="300">
<style>body{{font-family:system-ui,sans-serif;margin:24px;background:#f6f7f9}}table{{border-collapse:collapse;background:#fff}}
td,th{{padding:6px 10px;border-bottom:1px solid #e3e3e3;text-align:left;vertical-align:top}}th{{background:#222;color:#fff}}
.r{{font-size:12px;max-width:420px}}small{{color:#666}}</style></head><body>
<h2>Hard Drive Monitor</h2><small>{html.escape(time.strftime('%Y-%m-%d %H:%M:%S'))} &middot; source: {html.escape(str(meta.get('source')))}
&middot; <a href="/api/status">JSON</a> &middot; early warning only, keep backups</small><br><br>
<table><tr><th>Device</th><th>Model</th><th>Serial</th><th>Type</th><th>Temp</th><th>Power-on</th><th>Health</th>
<th>Perf</th><th>Status</th><th>Health history</th><th>Reasons</th></tr>{''.join(rows)}</table></body></html>"""


def serve(args, cfg) -> int:
    state: Dict[str, Any] = {"t": 0.0, "data": None}
    lock = threading.Lock()

    def snapshot():
        with lock:
            if state["data"] is None or time.time() - state["t"] > args.cache:
                results, meta = collect(cfg, args.from_json)
                hist: Dict[str, List[Dict[str, Any]]] = {}
                db = args.db or default_db()
                try:
                    with History(db) as h:
                        for d, _ in results:
                            hist[d.key] = h.readings(d.key, 48)
                except Exception:
                    pass
                state.update(t=time.time(), data=(results, meta, hist))
            return state["data"]

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            try:
                results, meta, hist = snapshot()
            except Exception as e:  # e.g. smartctl missing
                self._send(500, "text/plain", f"Hard Drive Monitor error: {e}")
                return
            if self.path.startswith("/api/status"):
                body = json.dumps({"meta": meta, "drives": [{"drive": d.to_dict(), "assessment": a.to_dict()}
                                                            for d, a in results]}, default=str)
                self._send(200, "application/json", body)
            elif self.path in ("/", "/index.html"):
                self._send(200, "text/html; charset=utf-8", render_page(results, hist, meta))
            else:
                self._send(404, "text/plain", "not found")

        def _send(self, code, ctype, body):
            data = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *a):
            pass

    srv = ThreadingHTTPServer((BIND, args.port), Handler)
    print(f"Hard Drive Monitor dashboard on http://{BIND}:{args.port}/  (Ctrl-C to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0
