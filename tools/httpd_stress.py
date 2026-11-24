#!/usr/bin/env python3
"""httpd stress tester for Muzix (Z80 + W5100).

Only the standard library is used.  Works against the emulator (TCP proxy on
the W5100 model) or against a real board (192.168.0.50:80).

Each request prints: t_connect, t_first_byte, t_total and outcome type
(connect-timeout / refused / accepted-no-reply / reset / empty / ok).

After every scenario iteration a control GET with a 3 s socket timeout is
done.  The control GET retries connect() within the timeout because
sys_park() creates halt windows where a connect can arrive but httpd
won't process it until the next pass (~83 ms).  That is not a hang.
"""
import argparse
import random
import socket
import ssl
import struct
import sys
import threading
import time

CONNECT_TIMEOUT = 3.0
RECV_TIMEOUT = 3.0
WATCHDOG_TOTAL = 300.0
RECV_CHUNK = 2048
G_PATH = "/doc/copy-notes.txt"


def _err_class(exc):
    msg = str(exc)
    if isinstance(exc, ConnectionRefusedError) or "refused" in msg.lower() or "ECONNREFUSED" in msg:
        return "refused"
    if isinstance(exc, socket.timeout) or "timed out" in msg.lower() or "ETIMEDOUT" in msg:
        return "timeout"
    if "reset" in msg.lower() or "ECONNRESET" in msg:
        return "reset"
    return "other"


def control_get(host, port):
    """Return True if a plain GET /doc/copy-notes.txt gets an HTTP response."""
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
            time.sleep(0.05)
            continue
        try:
            s.sendall(b"GET /doc/copy-notes.txt HTTP/1.0\r\nHost: %s\r\n\r\n" % host.encode())
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
        time.sleep(0.05)
    return False, last_err


def _read_response(s):
    try:
        s.settimeout(RECV_TIMEOUT)
        while True:
            chunk = s.recv(RECV_CHUNK)
            if not chunk:
                break
    except OSError:
        pass


def _time_request(s, path):
    """Perform a GET and return (t_connect, t_first_byte, t_total, outcome, data_len).
    t_connect and t_first_byte are measured from the moment we enter this function;
    the connect() itself has already completed by the caller."""
    t0 = time.time()
    req = ("GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n" % path).encode()
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


def scenario_a(s, host, port):
    t_conn, t_first, t_total, outcome, dlen = _time_request(s, G_PATH)
    print("  t_connect=%.3f t_first_byte=%.3f t_total=%.3f outcome=%s len=%d"
          % (t_conn or 0, t_first or 0, t_total or 0, outcome, dlen), flush=True)


def scenario_b(s, host, port):
    # connect + immediate close, no data
    pass


def scenario_c(s, host, port):
    # connect + silence 5 s + close (H3 reproduction)
    time.sleep(5.0)


def scenario_d(s, host, port):
    # half request + close
    s.sendall(b"GET /")


def scenario_e(s, host, port):
    # RST instead of FIN (SO_LINGER 0)
    s.sendall(b"GET /doc/copy-notes.txt HTTP/1.0\r\n\r\n")
    _read_response(s)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("hh", 1, 0))
    except OSError:
        pass


def scenario_f(s, host, port):
    s.sendall(b"GET /nonexistent HTTP/1.0\r\nHost: %s\r\n\r\n" % host.encode())
    _read_response(s)


def scenario_g(s, host, port):
    name = random.choice("abcdef")
    SCENARIOS[name](s, host, port)


def scenario_h(s, host, port):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        ss = ctx.wrap_socket(s, server_hostname=host)
        ss.settimeout(RECV_TIMEOUT)
        try:
            ss.recv(4096)
        except (ssl.SSLError, OSError, socket.timeout):
            pass
        ss.close()
    except (ssl.SSLError, OSError):
        pass
    try:
        s.close()
    except OSError:
        pass


_TLS_CLIENTHELLO_200 = bytes.fromhex(
    "16030100c2010000be0303" + "".join("%02x" % i for i in range(32)) +
    "00" + "0a000a0000" +
    "00" + "13011302130313041305" +
    "000a00080006001d001700180019" +
    "01000101" +
    "000b000403000102" +
    "000a0006000400170018" +
    "000d00080016001700180019" +
    "000e000600170018" +
    "000500050100000000" +
    "000c000800160017001800190002" +
    "000f000101" +
    "00120000" +
    "00170000" +
    "00180000" +
    "00190000" +
    "00230000" +
    "002b0003020304" +
    "002d00020101" +
    "002f0000" +
    "00330000" +
    "00350000" +
    "00370000" +
    "003d0000" +
    "003e0000" +
    "003f0000" +
    "00400000" +
    "00410000" +
    "00420000" +
    "00430000" +
    "00440000" +
    "00450000" +
    "00460000" +
    "00ff01000100" +
    "000a00080006001d001700180019" +
    "000b000403000102" +
    "000403000102" +
    "000d00080016001700180019" +
    "000e000600170018" +
    "00020100" +
    "000a000600170018" +
    "000d00080016001700180019" +
    "000e000600170018" +
    "00020100"
)

_TLS_CLIENTHELLO_517 = _TLS_CLIENTHELLO_200[:517]


def scenario_i(s, host, port, size):
    data = bytes([(i * 7 + 3) % 256 for i in range(size)])
    try:
        s.sendall(data)
    except OSError:
        pass


def scenario_j(s, host, port, field_type, size):
    if field_type == "path":
        s.sendall(("GET /" + "A" * size + " HTTP/1.0\r\nHost: x\r\n\r\n").encode())
    elif field_type == "ua":
        s.sendall(("GET / HTTP/1.0\r\nHost: x\r\nUser-Agent: " + "A" * size + "\r\n\r\n").encode())
    elif field_type == "request_line":
        s.sendall(("GET /" + "A" * size + " HTTP/1.0\r\n\r\n").encode())
    _read_response(s)


def scenario_k(s, host, port, variant):
    if variant == "nul_method":
        s.sendall(b"GET\x00 / HTTP/1.0\r\n\r\n")
    elif variant == "high_path":
        s.sendall(b"GET /\x80\xff\xfe HTTP/1.0\r\n\r\n")
    elif variant == "no_method":
        s.sendall(b" / HTTP/1.0\r\n\r\n")
    elif variant == "just_crlf":
        s.sendall(b"\r\n\r\n")
    elif variant == "keepalive":
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n")
    elif variant == "head":
        s.sendall(b"HEAD /doc/copy-notes.txt HTTP/1.0\r\n\r\n")
    elif variant == "post":
        s.sendall(b"POST / HTTP/1.0\r\nContent-Length: 5\r\n\r\nhello")
    elif variant == "options":
        s.sendall(b"OPTIONS / HTTP/1.0\r\n\r\n")
    _read_response(s)


def scenario_l(s, host, port):
    req = b"GET /doc/copy-notes.txt HTTP/1.0\r\nHost: x\r\n\r\n"
    for b in req:
        try:
            s.send(bytes([b]))
        except OSError:
            break
        time.sleep(1.0)
    _read_response(s)


SCENARIOS = {
    "a": scenario_a,
    "b": scenario_b,
    "c": scenario_c,
    "d": scenario_d,
    "e": scenario_e,
    "f": scenario_f,
    "g": scenario_g,
    "h": scenario_h,
}


def run_sequential(host, port, scenarios, n_iter, wait_on_fail):
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
                SCENARIOS[name](s, host, port)
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
            ok, err = control_get(host, port)
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


def run_concurrency(host, port, n, hold_sec):
    results = [None] * n
    threads = []
    for i in range(n):
        t = threading.Thread(target=hold_one, args=(results, i, host, port, hold_sec))
        t.start()
        threads.append(t)
    time.sleep(max(0.5, hold_sec))
    responded, err = control_get(host, port)
    for t in threads:
        t.join()
    if not responded:
        print("CONCURRENCY: %d simultaneous silent clients held; control GET FAILED "
              "(%s) - server not accepting (no LISTEN socket)" % (n, err), flush=True)
        return 1
    print("CONCURRENCY: %d simultaneous silent clients held; control GET OK" % n, flush=True)
    return 0


def main():
    ap = argparse.ArgumentParser(description="Stress-test Muzix httpd")
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--iterations", type=int, default=40)
    ap.add_argument("--scenarios", default="abcdefg",
                    help="scenarios to run sequentially (default abcdefg)")
    ap.add_argument("--concurrency", type=int, default=0,
                    help="if >0, hold this many silent clients concurrently then control GET")
    ap.add_argument("--hold", type=float, default=5.0,
                    help="seconds to hold concurrent silent clients (default 5)")
    ap.add_argument("--wait-on-fail", action="store_true",
                    help="wait 60 s on first failure to see if it recovers by itself")
    ap.add_argument("--path", default="/doc/copy-notes.txt",
                    help="URL path to request (default /doc/copy-notes.txt)")
    args = ap.parse_args()

    global G_PATH
    G_PATH = args.path

    if args.concurrency > 0:
        return run_concurrency(args.host, args.port, args.concurrency, args.hold)

    return run_sequential(args.host, args.port, args.scenarios, args.iterations, args.wait_on_fail)


if __name__ == "__main__":
    sys.exit(main())
