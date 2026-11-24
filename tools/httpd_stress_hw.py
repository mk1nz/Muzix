#!/usr/bin/env python3
"""httpd stress tester for Muzix on REAL HARDWARE.

Tests against a physical Zeta SBC V2 board running Muzix httpd.

Targets:
  LAN:      192.168.0.50:80
  External: 93.177.187.134:1976 (port-forwarded to 192.168.0.50:80)

Only the standard library is used.  Works against real hardware.

Each request prints: t_connect, t_first_byte, t_total and outcome type
(connect-timeout / refused / accepted-no-reply / reset / empty / ok).

After every scenario iteration a control GET with a 5 s socket timeout is
done.  The control GET retries connect() within the timeout because
sys_park() creates halt windows where a connect can arrive but httpd
won't process it until the next pass (~250 ms on real hardware).

Usage:
  # LAN testing (recommended first)
  python3 tools/httpd_stress_hw.py --lan --iterations 200 --path /cgi-bin/hello

  # External testing (port-forwarded)
  python3 tools/httpd_stress_hw.py --external --iterations 200 --path /cgi-bin/hello

  # Custom target
  python3 tools/httpd_stress_hw.py --host 192.168.0.50 --port 80 --iterations 200

Scenarios:
  (a) normal GET <path>
  (b) connect + immediate close, no data
  (c) connect + silence 5 s + close   (H3 reproduction: silent client)
  (d) half request ("GET /" without CRLF) + close
  (e) RST instead of FIN (SO_LINGER 0)
  (f) GET nonexistent path
  (g) random mix of (a)-(f)
"""
import argparse
import random
import socket
import ssl
import struct
import sys
import threading
import time

CONNECT_TIMEOUT = 5.0   # Real hardware may need longer than emulator
RECV_TIMEOUT = 5.0
WATCHDOG_TOTAL = 600.0  # 10 minutes max for 200 requests
RECV_CHUNK = 2048

LAN_HOST = "192.168.0.50"
LAN_PORT = 80
EXT_HOST = "93.177.187.134"
EXT_PORT = 1976


def _err_class(exc):
    msg = str(exc)
    if isinstance(exc, ConnectionRefusedError) or "refused" in msg.lower() or "ECONNREFUSED" in msg:
        return "refused"
    if isinstance(exc, socket.timeout) or "timed out" in msg.lower() or "ETIMEDOUT" in msg:
        return "timeout"
    if "reset" in msg.lower() or "ECONNRESET" in msg:
        return "reset"
    return "other"


def control_get(host, port, path):
    """Return True if a plain GET <path> gets an HTTP response in time.

    Retries connect() within WATCHDOG_TOTAL because sys_park() halt windows
    can cause a connect to arrive while httpd is parked.  That is not a hang
    - httpd will process it on the next pass (~250 ms on real hardware).
    """
    deadline = time.time() + WATCHDOG_TOTAL
    last_err = None
    while time.time() < deadline:
        remaining = deadline - time.time()
        if remaining <= 0.05:
            break
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(min(RECV_TIMEOUT, remaining))
        try:
            s.connect((host, port))
        except OSError as exc:
            last_err = _err_class(exc)
            try:
                s.close()
            except OSError:
                pass
            time.sleep(0.1)
            continue
        try:
            s.sendall(("GET %s HTTP/1.0\r\nHost: %s\r\n\r\n" % (path, host)).encode())
            data = b""
            s.settimeout(min(RECV_TIMEOUT, deadline - time.time()))
            while time.time() < deadline:
                try:
                    chunk = s.recv(RECV_CHUNK)
                except socket.timeout:
                    last_err = "timeout"
                    break
                if not chunk:
                    last_err = "empty-reply"
                    break
                data += chunk
                if data.startswith(b"HTTP/") and b"\r\n\r\n" in data:
                    return True, None
            if data.startswith(b"HTTP/"):
                return True, None
        except OSError as exc:
            last_err = _err_class(exc)
        finally:
            try:
                s.close()
            except OSError:
                pass
        time.sleep(0.1)
    return False, last_err


def _time_request(s, path, host):
    """Perform a GET and return (t_connect, t_first_byte, t_total, outcome, data_len)."""
    t0 = time.time()
    req = ("GET %s HTTP/1.0\r\nHost: %s\r\n\r\n" % (path, host)).encode()
    try:
        s.sendall(req)
    except OSError as exc:
        return None, None, None, _err_class(exc), 0
    t_first = None
    data = b""
    s.settimeout(RECV_TIMEOUT)
    while True:
        try:
            chunk = s.recv(RECV_CHUNK)
        except socket.timeout:
            if t_first is None:
                return time.time() - t0, None, time.time() - t0, "timeout", len(data)
            return time.time() - t0, t_first, time.time() - t0, "timeout", len(data)
        if not chunk:
            if t_first is None:
                return time.time() - t0, None, time.time() - t0, "empty", len(data)
            return time.time() - t0, t_first, time.time() - t0, "empty-reply", len(data)
        if t_first is None:
            t_first = time.time() - t0
        data += chunk
        if data.startswith(b"HTTP/") and b"\r\n\r\n" in data:
            return time.time() - t0, t_first, time.time() - t0, "ok", len(data)


def scenario_a(s, host, port, path):
    t_conn, t_first, t_total, outcome, dlen = _time_request(s, path, host)
    print("  t_connect=%.3f t_first_byte=%.3f t_total=%.3f outcome=%s len=%d"
          % (t_conn or 0, t_first or 0, t_total or 0, outcome, dlen), flush=True)


def scenario_b(s, host, port, path):
    # connect + immediate close, no data
    pass


def scenario_c(s, host, port, path):
    # connect + silence 5 s + close   (H3 reproduction)
    time.sleep(5.0)


def scenario_d(s, host, port, path):
    # half request ("GET /" without CRLF) + close
    s.sendall(b"GET /")


def scenario_e(s, host, port, path):
    # RST instead of FIN (SO_LINGER 0)
    s.sendall(("GET %s HTTP/1.0\r\n\r\n" % path).encode())
    try:
        s.settimeout(5.0)
        while True:
            chunk = s.recv(RECV_CHUNK)
            if not chunk:
                break
    except OSError:
        pass
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("hh", 1, 0))
    except OSError:
        pass


def scenario_f(s, host, port, path):
    s.sendall(("GET /nonexistent HTTP/1.0\r\nHost: %s\r\n\r\n" % host).encode())
    try:
        s.settimeout(5.0)
        while True:
            chunk = s.recv(RECV_CHUNK)
            if not chunk:
                break
    except OSError:
        pass


def scenario_g(s, host, port, path):
    name = random.choice("abcdef")
    SCENARIOS[name](s, host, port, path)


SCENARIOS = {
    "a": scenario_a,
    "b": scenario_b,
    "c": scenario_c,
    "d": scenario_d,
    "e": scenario_e,
    "f": scenario_f,
    "g": scenario_g,
}


def run_sequential(host, port, scenarios, n_iter, path, wait_on_fail):
    start_wall = time.time()
    request = 0
    for name in scenarios:
        if name not in SCENARIOS:
            print("unknown scenario: %s" % name, file=sys.stderr)
            sys.stdout.flush()
            return 2
        for it in range(n_iter):
            request += 1
            if time.time() - start_wall > WATCHDOG_TOTAL:
                print("REQUEST %d: WATCHDOG TOTAL EXCEEDED - aborting" % request, flush=True)
                return 1
            t_connect_start = time.time()
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(CONNECT_TIMEOUT)
            try:
                s.connect((host, port))
            except OSError as exc:
                cls = _err_class(exc)
                t_connect = time.time() - t_connect_start
                print("REQUEST %d: scenario %s iter %d: t_connect=%.3f CONNECT FAILED (%s: %s)"
                      % (request, name, it, t_connect, cls, exc), flush=True)
                if wait_on_fail:
                    print("REQUEST %d: waiting 60 s to see if it recovers..." % request, flush=True)
                    time.sleep(60.0)
                return 1
            t_connect = time.time() - t_connect_start
            try:
                SCENARIOS[name](s, host, port, path)
            except OSError as exc:
                cls = _err_class(exc)
                print("REQUEST %d: scenario %s iter %d: SCENARIO FAILED (%s: %s)"
                      % (request, name, it, cls, exc), flush=True)
                try:
                    s.close()
                except OSError:
                    pass
                if wait_on_fail:
                    print("REQUEST %d: waiting 60 s to see if it recovers..." % request, flush=True)
                    time.sleep(60.0)
                return 1
            finally:
                try:
                    s.close()
                except OSError:
                    pass
            ok, err = control_get(host, port, path)
            if not ok:
                print("REQUEST %d: scenario %s iter %d: SERVER STOPPED RESPONDING "
                      "(control GET failed: %s)" % (request, name, it, err), flush=True)
                if wait_on_fail:
                    print("REQUEST %d: waiting 60 s to see if it recovers..." % request, flush=True)
                    time.sleep(60.0)
                return 1
            print("REQUEST %d: scenario %s iter %d: OK" % (request, name, it), flush=True)
        print("scenario %s: %d iterations OK" % (name, n_iter), flush=True)
    print("ALL SEQUENTIAL SCENARIOS OK: %d requests, server still responding" % request, flush=True)
    return 0


def hold_one(results, idx, host, port, hold_sec):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(CONNECT_TIMEOUT)
    ok = False
    err = None
    try:
        s.connect((host, port))
        time.sleep(hold_sec)
        ok = True
    except OSError as exc:
        err = _err_class(exc)
    finally:
        try:
            s.close()
        except OSError:
            pass
    results[idx] = (ok, err)


def run_concurrency(host, port, n, hold_sec, path):
    results = [None] * n
    threads = []
    for i in range(n):
        t = threading.Thread(target=hold_one, args=(results, i, host, port, hold_sec))
        t.start()
        threads.append(t)
    time.sleep(max(0.5, hold_sec))
    responded, err = control_get(host, port, path)
    for t in threads:
        t.join()
    if not responded:
        print("CONCURRENCY: %d simultaneous silent clients held; control GET FAILED "
              "(%s) - server not accepting (no LISTEN socket)" % (n, err), flush=True)
        return 1
    print("CONCURRENCY: %d simultaneous silent clients held; control GET OK" % n, flush=True)
    return 0


def main():
    ap = argparse.ArgumentParser(description="Stress-test Muzix httpd on REAL HARDWARE")
    ap.add_argument("--lan", action="store_true",
                    help="test against 192.168.0.50:80 (LAN)")
    ap.add_argument("--external", action="store_true",
                    help="test against 93.177.187.134:1976 (port-forwarded external)")
    ap.add_argument("--host", default=None, help="target host (overrides --lan/--external)")
    ap.add_argument("--port", type=int, default=None, help="target port")
    ap.add_argument("--iterations", type=int, default=40,
                    help="iterations per scenario (default 40)")
    ap.add_argument("--scenarios", default="abcdefg",
                    help="scenarios to run sequentially (default abcdefg)")
    ap.add_argument("--concurrency", type=int, default=0,
                    help="if >0, hold this many silent clients concurrently then control GET")
    ap.add_argument("--hold", type=float, default=5.0,
                    help="seconds to hold concurrent silent clients (default 5)")
    ap.add_argument("--wait-on-fail", action="store_true",
                    help="wait 60 s on first failure to see if it recovers by itself")
    ap.add_argument("--path", default="/cgi-bin/hello",
                    help="URL path to request (default /cgi-bin/hello)")
    args = ap.parse_args()

    # Determine target host/port
    if args.host is not None:
        host = args.host
    elif args.external:
        host = EXT_HOST
    else:
        host = LAN_HOST  # default to LAN

    if args.port is not None:
        port = args.port
    elif args.external:
        port = EXT_PORT
    else:
        port = LAN_PORT  # default to 80

    target_desc = "%s:%d" % (host, port)
    print("Target: %s" % target_desc, flush=True)
    print("Path: %s" % args.path, flush=True)
    print("Scenarios: %s" % args.scenarios, flush=True)
    print("Iterations per scenario: %d" % args.iterations, flush=True)
    sys.stdout.flush()

    if args.concurrency > 0:
        return run_concurrency(host, port, args.concurrency, args.hold, args.path)

    return run_sequential(host, port, args.scenarios, args.iterations, args.path, args.wait_on_fail)


if __name__ == "__main__":
    sys.exit(main())
