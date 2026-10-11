"""Hard Drive Monitor command line (`hdmon`)."""
from __future__ import annotations

import argparse
import json
import sys
import time
from datetime import datetime
from typing import Any, Dict, List, Optional

from . import __version__
from .alerts import build_alerts, dispatch, notify_desktop, send_email
from .collect import collect, find_drive, get_smartctl
from .config import load_config, write_sample
from .health import RANK, assess
from .history import History
from .paths import config_path, default_db, os_name
from .render import Painter, detail_view, history_view, status_view
from .smart import DeviceRef, SmartctlMissing, discover, install_hint, read_drive

EXIT_CODE = {"OK": 0, "UNKNOWN": 0, "WARNING": 1, "CRITICAL": 2}


def _dump(obj) -> str:
    return json.dumps(obj, indent=2, default=str)


def _results_json(results, changes=None, meta=None) -> Dict[str, Any]:
    return {"version": __version__, "generated": datetime.now().astimezone().isoformat(timespec="seconds"),
            "meta": meta or {},
            "drives": [{"drive": d.to_dict(), "assessment": a.to_dict(),
                        "changes": (changes or {}).get(d.key, [])} for d, a in results]}


def _should_record(args) -> bool:
    if getattr(args, "no_record", False):
        return False
    return not args.from_json or getattr(args, "record", False)


def _gather(args, cfg):
    results, meta = collect(cfg, args.from_json)
    changes: Dict[str, list] = {}
    if _should_record(args):
        with History(args.db or default_db()) as h:
            changes = h.record(results, source=meta.get("source", "live"))
    return results, meta, changes


# ---------------------------------------------------------------- commands

def cmd_status(args, cfg, paint) -> int:
    results, meta, changes = _gather(args, cfg)
    if args.json:
        print(_dump(_results_json(results, changes, meta)))
    else:
        print(status_view(results, paint, meta, changes))
    return 0


def cmd_detail(args, cfg, paint) -> int:
    results, meta = collect(cfg, args.from_json)
    hit = find_drive(results, args.device)
    if hit is None and not args.from_json:
        sc = get_smartctl(cfg)
        if sc:  # not in the scan (e.g. USB bridge): try it directly
            d = read_drive(sc, DeviceRef(args.device, args.type))
            hit = (d, assess(d, cfg.get("thresholds")))
    if hit is None:
        print(f"hdmon: no drive matching {args.device!r}. Known: "
              + ", ".join(d.device for d, _ in results), file=sys.stderr)
        return 3
    d, a = hit
    if args.json:
        print(_dump({"drive": d.to_dict(), "assessment": a.to_dict(), "meta": meta}))
    else:
        print(detail_view(d, a, paint))
    return 0


def cmd_history(args, cfg, paint) -> int:
    path = args.db or default_db()
    if not args.db and not default_db().exists():
        summary: List[Dict[str, Any]] = []
    else:
        with History(path) as h:
            summary = h.summary(limit=args.limit, drive_filter=args.drive)
    if args.json:
        print(_dump({"db": str(path), "drives": summary}))
    else:
        print(f"History: {path}\n")
        print(history_view(summary, paint))
    return 0


def cmd_check(args, cfg, paint, once_log=None) -> int:
    results, meta, changes = _gather(args, cfg)
    alerts = build_alerts(results, changes)
    sent = dispatch(cfg, alerts) if not args.no_alert else []
    worst = max((a.status for _, a in results), key=lambda s: RANK[s], default="OK")
    if args.json:
        out = _results_json(results, changes, meta)
        out.update(alerts=alerts, alert_log=sent, worst=worst)
        print(json.dumps(out, default=str) if getattr(args, "_jsonl", False) else _dump(out))
    elif not args.quiet:
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{ts}] {len(results)} drive(s), worst status {worst}, {len(alerts)} new alert(s)")
        for al in alerts:
            print(f"  ALERT {al['title']}\n    " + al["body"].replace("\n", "\n    "))
        for line in sent:
            print("  " + line)
    elif alerts:
        for al in alerts:
            print(f"ALERT {al['title']}: " + al["body"].replace("\n", " | "))
    return EXIT_CODE[worst]


def cmd_watch(args, cfg, paint) -> int:
    from .service import parse_interval
    interval = parse_interval(args.interval)
    args._jsonl = True
    n = 0
    rc = 0
    try:
        while True:
            rc = cmd_check(args, cfg, paint)
            sys.stdout.flush()
            n += 1
            if args.count and n >= args.count:
                break
            time.sleep(interval)
    except KeyboardInterrupt:
        pass
    return rc


def cmd_selftest(args, cfg, paint) -> int:
    if args.from_json:
        if not args.results:
            print("hdmon: --from-json can only show --results, not start a test", file=sys.stderr)
            return 3
        results, _ = collect(cfg, args.from_json)
        hit = find_drive(results, args.device)
        if not hit:
            print(f"hdmon: no drive matching {args.device!r}", file=sys.stderr)
            return 3
        return _print_selftest(hit[0], args, paint)
    sc = get_smartctl(cfg)
    if sc is None:
        print("smartctl (smartmontools) is needed for self-tests. Install it with:\n  " + install_hint(), file=sys.stderr)
        return 3
    refs = discover(sc)
    ref = next((r for r in refs if args.device in (r.name, r.name.rsplit("/", 1)[-1])), None) \
        or DeviceRef(args.device, args.type)
    d = read_drive(sc, ref)
    if args.results:
        return _print_selftest(d, args, paint)
    if not d.smart_available:
        print(f"hdmon: cannot read SMART on {ref.name}: " + "; ".join(d.notes), file=sys.stderr)
        return 3
    st = d.self_test or {}
    if st.get("in_progress"):
        msg = f"A self-test is already running on {ref.name} ({st.get('remaining_percent')}% remaining). " \
              f"Abort it with: smartctl -X {ref.name}"
        print(_dump({"started": False, "reason": msg}) if args.json else msg)
        return 1
    minutes = st.get("short_minutes") if args.kind == "short" else st.get("extended_minutes")
    if minutes:
        expected = f"about {minutes} minute(s)" + (f" (~{minutes / 60:.1f} h)" if minutes >= 90 else "")
    elif d.kind == "NVMe" and args.kind == "short":
        expected = "up to 2 minutes (NVMe spec limit)"
    else:
        expected = "unknown - the drive did not report it (NVMe extended tests are typically 10-90 min)"
    cmd = ["-t", args.kind, "--json"] + (["-d", ref.dev_type] if ref.dev_type else []) + [ref.name]
    rc, data, err = sc.run(cmd, timeout=60)
    from .smart import messages_text
    ok = not (rc & 0x07)
    info = messages_text(data) or err.strip()
    if args.json:
        print(_dump({"started": ok, "device": ref.name, "kind": args.kind, "expected_duration": expected,
                     "expected_minutes": minutes, "smartctl_exit": rc, "message": info}))
    elif ok:
        print(f"Started {args.kind} self-test on {ref.name} ({d.model or ''}). Expected duration: {expected}.\n"
              f"The drive stays usable meanwhile (it may be slower). Check later with:\n"
              f"  hdmon selftest {args.device} --results")
    else:
        print(f"Self-test did not start on {ref.name} (smartctl exit {rc}): {info}", file=sys.stderr)
    return 0 if ok else 1


def _print_selftest(d, args, paint) -> int:
    if args.json:
        print(_dump({"device": d.device, "current": d.self_test, "log": d.self_test_log}))
        return 0
    st = d.self_test or {}
    print(f"{d.device}  {d.model or ''}")
    print(f"  Current status: {st.get('status') or '-'}"
          + (f"  ({st.get('remaining_percent')}% remaining)" if st.get("in_progress") else ""))
    if st.get("short_minutes") or st.get("extended_minutes"):
        print(f"  Typical duration: short {st.get('short_minutes') or '?'} min, "
              f"long {st.get('extended_minutes') or '?'} min")
    if not d.self_test_log:
        print("  Self-test log is empty.")
    for row in d.self_test_log:
        col = "green" if row.get("passed") else "red"
        lba = f"  first error LBA {row['lba']}" if row.get("lba") not in (None, "-") else ""
        print(f"  {str(row.get('type')):<18}{paint(str(row.get('status')).ljust(40), col)}  at {row.get('lifetime_hours')} h{lba}")
    return 0


def cmd_install(args, cfg, paint) -> int:
    from . import service
    lines: List[str] = []
    out = lines.append if args.json else print
    rc = service.install(service.parse_interval(args.interval), enable=not args.no_enable, system=args.system,
                         highest=args.highest, dry_run=args.dry_run, out=out)
    if args.json:
        print(_dump({"ok": rc == 0, "output": lines}))
    return rc


def cmd_uninstall(args, cfg, paint) -> int:
    from . import service
    lines: List[str] = []
    rc = service.uninstall(system=args.system, out=lines.append if args.json else print)
    if args.json:
        print(_dump({"ok": rc == 0, "output": lines}))
    return rc


def cmd_serve(args, cfg, paint) -> int:
    from .web import serve
    return serve(args, cfg)


def cmd_init_config(args, cfg, paint) -> int:
    try:
        p = write_sample(args.config, force=args.force)
    except FileExistsError as e:
        msg = f"{e} already exists (use --force to overwrite)"
        print(_dump({"ok": False, "error": msg}) if args.json else msg)
        return 1
    print(_dump({"ok": True, "path": str(p)}) if args.json else f"Wrote {p}")
    return 0


def cmd_alert_test(args, cfg, paint) -> int:
    title, body = "Hard Drive Monitor test alert", "If you can read this, Hard Drive Monitor alerts work."
    ok, info = notify_desktop(title, body)
    res = {"desktop": {"ok": ok, "info": info}}
    if (cfg.get("email") or {}).get("enabled"):
        ok2, info2 = send_email(cfg, title, body)
        res["email"] = {"ok": ok2, "info": info2}
    print(_dump(res) if args.json else "\n".join(f"{k}: {'ok' if v['ok'] else 'FAILED'} ({v['info']})" for k, v in res.items()))
    return 0


# ---------------------------------------------------------------- parser

def build_parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--json", action="store_true", help="machine-readable JSON output")
    common.add_argument("--color", choices=["auto", "always", "never"], default="auto")
    common.add_argument("--config", help=f"config file (default {config_path()})")
    common.add_argument("--db", help=f"history database (default {default_db()})")
    src = argparse.ArgumentParser(add_help=False)
    src.add_argument("--from-json", action="append", metavar="FILE_OR_DIR",
                     help="read saved `smartctl -x --json` output instead of live drives (repeatable)")
    rec = argparse.ArgumentParser(add_help=False)
    rec.add_argument("--no-record", action="store_true", help="do not write this run to the history DB")
    rec.add_argument("--record", action="store_true", help="with --from-json: also record to history")

    p = argparse.ArgumentParser(prog="hdmon", description="Hard Drive Monitor - SMART drive health monitor (smartmontools based) "
                                "for Linux, macOS and Windows. Early warning only - keep backups.")
    p.add_argument("--version", action="version", version=f"Hard Drive Monitor (hdmon) {__version__}")
    sub = p.add_subparsers(dest="cmd", metavar="COMMAND")
    sub.required = True

    s = sub.add_parser("status", parents=[common, src, rec], help="colored table of all drives")
    s.set_defaults(func=cmd_status)

    s = sub.add_parser("detail", parents=[common, src], help="full attribute view of one drive")
    s.add_argument("device", help="e.g. /dev/sda, sda, nvme0, disk0, D: or a serial number")
    s.add_argument("-d", "--type", help="smartctl -d device type (e.g. sat, nvme) if auto-detect fails")
    s.set_defaults(func=cmd_detail)

    s = sub.add_parser("history", parents=[common], help="trends from the local SQLite log")
    s.add_argument("--limit", type=int, default=30, help="readings per drive (default 30)")
    s.add_argument("--drive", help="only this device/serial")
    s.set_defaults(func=cmd_history)

    s = sub.add_parser("check", parents=[common, src, rec],
                       help="one scheduled-style run: record, alert on worsening, exit 0/1/2 = OK/WARNING/CRITICAL")
    s.add_argument("--quiet", action="store_true")
    s.add_argument("--no-alert", action="store_true", help="record but do not notify")
    s.set_defaults(func=cmd_check)

    s = sub.add_parser("watch", parents=[common, src, rec], help="loop: check every --interval")
    s.add_argument("--interval", default="1h", help="e.g. 1h, 30m, 2h30m (default 1h)")
    s.add_argument("--count", type=int, default=0, help="stop after N runs (default: forever)")
    s.add_argument("--quiet", action="store_true")
    s.add_argument("--no-alert", action="store_true")
    s.set_defaults(func=cmd_watch)

    s = sub.add_parser("selftest", parents=[common, src], help="start a SMART self-test or show results")
    s.add_argument("device")
    s.add_argument("kind", nargs="?", choices=["short", "long"], default="short")
    s.add_argument("--results", action="store_true", help="show the self-test log instead of starting a test")
    s.add_argument("-d", "--type", help="smartctl -d device type")
    s.set_defaults(func=cmd_selftest)

    for name, fn, h in (("install-service", cmd_install, "schedule hourly checks (systemd user timer / launchd / Task Scheduler)"),
                        ("uninstall-service", cmd_uninstall, "remove the scheduled check")):
        s = sub.add_parser(name, parents=[common], help=h)
        s.add_argument("--system", action="store_true",
                       help="root/system-wide: print the files and commands only, never installed silently")
        if name == "install-service":
            s.add_argument("--interval", default="1h")
            s.add_argument("--no-enable", action="store_true", help="write the files but do not enable them")
            s.add_argument("--dry-run", action="store_true", help="print, write nothing")
            s.add_argument("--highest", action="store_true", help="Windows: task runs with highest privileges (/RL HIGHEST)")
        s.set_defaults(func=fn)

    s = sub.add_parser("serve", parents=[common, src], help="local web dashboard on 127.0.0.1")
    s.add_argument("--port", type=int, default=8765)
    s.add_argument("--cache", type=int, default=60, help="seconds between live rescans (default 60)")
    s.set_defaults(func=cmd_serve)

    s = sub.add_parser("init-config", parents=[common], help="write a sample config.toml")
    s.add_argument("--force", action="store_true")
    s.set_defaults(func=cmd_init_config)

    s = sub.add_parser("alert-test", parents=[common], help="send a test desktop notification (and e-mail if enabled)")
    s.set_defaults(func=cmd_alert_test)
    return p


def main(argv: Optional[List[str]] = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")  # never crash on a cp1252 console
        except (AttributeError, ValueError):
            pass
    args = build_parser().parse_args(argv)
    try:
        cfg = load_config(args.config)
    except Exception as e:  # malformed TOML
        print(f"hdmon: cannot read config: {e}", file=sys.stderr)
        return 3
    paint = Painter("never" if args.json else args.color)
    try:
        return args.func(args, cfg, paint)
    except SmartctlMissing as e:
        if getattr(args, "json", False):
            print(_dump({"error": str(e), "install": install_hint()}))
        else:
            print(f"hdmon: {e}", file=sys.stderr)
        return 3
    except KeyboardInterrupt:
        return 130
    except BrokenPipeError:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
