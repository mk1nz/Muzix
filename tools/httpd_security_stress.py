#!/usr/bin/env python3
"""
httpd_security_stress.py — аудит защиты Muzix HTTPD от переполнений
и чрезмерно больших запросов.

Тестирует эмулятор. Для реальной платы используйте --host/--port.

Каждый тест:
1. Подключается к серверу
2. Отправляет специально созданный запрос
3. Замеряет t_connect, t_first_byte, t_total, HTTP status, размер ответа
4. После каждого негативного сценария делает контрольный GET /cgi-bin/hello
   для проверки восстановления сервера

Выходной код: 0 если все тесты прошли (сервер восстанавливается),
1 если сервер завис после какого-то теста.
"""
import argparse
import random
import socket
import sys
import time

CONNECT_TIMEOUT = 5.0
RECV_TIMEOUT = 5.0
CONTROL_TIMEOUT = 5.0
RECV_CHUNK = 2048


def err_class(exc):
    msg = str(exc)
    if isinstance(exc, ConnectionRefusedError) or "refused" in msg.lower() or "ECONNREFUSED" in msg:
        return "refused"
    if isinstance(exc, socket.timeout) or "timed out" in msg.lower() or "ETIMEDOUT" in msg:
        return "timeout"
    if "reset" in msg.lower() or "ECONNRESET" in msg:
        return "reset"
    return "other"


def control_get(host, port):
    """Контрольный GET /cgi-bin/hello. Возвращает (ok, err_class)."""
    deadline = time.time() + CONTROL_TIMEOUT
    while time.time() < deadline:
        remaining = deadline - time.time()
        if remaining <= 0.05:
            break
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(min(RECV_TIMEOUT, remaining))
        try:
            s.connect((host, port))
        except OSError as exc:
            try:
                s.close()
            except OSError:
                pass
            time.sleep(0.1)
            continue
        try:
            s.sendall(b"GET /cgi-bin/hello HTTP/1.0\r\nHost: x\r\n\r\n")
            data = b""
            s.settimeout(min(RECV_TIMEOUT, deadline - time.time()))
            while time.time() < deadline:
                try:
                    chunk = s.recv(RECV_CHUNK)
                except socket.timeout:
                    return False, "timeout"
                if not chunk:
                    return False, "empty-reply"
                data += chunk
                if data.startswith(b"HTTP/") and b"\r\n\r\n" in data:
                    return True, None
            if data.startswith(b"HTTP/"):
                return True, None
        except OSError as exc:
            pass
        finally:
            try:
                s.close()
            except OSError:
                pass
        time.sleep(0.1)
    return False, "timeout"


def timed_request(s, send_bytes, expect_response=True, timeout=RECV_TIMEOUT):
    """Отправляет send_bytes, замеряет тайминги.
    Возвращает (t_connect, t_first_byte, t_total, status_code, resp_len, err_class).
    t_connect измеряется вызывающим до вызова этой функции.
    """
    t0 = time.time()
    t_first = None
    data = b""
    try:
        s.sendall(send_bytes)
    except OSError as exc:
        return None, None, None, None, 0, err_class(exc)

    s.settimeout(timeout)
    while True:
        try:
            chunk = s.recv(RECV_CHUNK)
        except socket.timeout:
            if t_first is None:
                return time.time() - t0, None, time.time() - t0, None, len(data), "timeout"
            return time.time() - t0, t_first, time.time() - t0, None, len(data), "timeout"
        if not chunk:
            if t_first is None:
                return time.time() - t0, None, time.time() - t0, None, len(data), "empty-reply"
            return time.time() - t0, t_first, time.time() - t0, None, len(data), "empty-reply"
        if t_first is None:
            t_first = time.time() - t0
        data += chunk
        if data.startswith(b"HTTP/") and b"\r\n\r\n" in data:
            # Извлекаем статус-код
            header_end = data.find(b"\r\n\r\n")
            header_part = data[:header_end].decode("latin-1", errors="replace")
            status_line = header_part.split("\r\n")[0] if header_part else ""
            parts = status_line.split(" ")
            status_code = parts[1] if len(parts) >= 2 else "?"
            return time.time() - t0, t_first, time.time() - t0, status_code, len(data), None
    return time.time() - t0, t_first, time.time() - t0, None, len(data), "unknown"


def run_case(name, send_bytes, host, port, timeout=RECV_TIMEOUT, expect_recovery=True):
    """Один тестовый случай. Возвращает (ok, details_str)."""
    print(f"  TEST: {name}", flush=True)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(CONNECT_TIMEOUT)
    t_connect_start = time.time()
    try:
        s.connect((host, port))
    except OSError as exc:
        t_connect = time.time() - t_connect_start
        print(f"    CONNECT FAILED: {err_class(exc)} (t_connect={t_connect:.3f}s)", flush=True)
        try:
            s.close()
        except OSError:
            pass
        ok, err = control_get(host, port)
        print(f"    CONTROL GET after connect-fail: {'OK' if ok else 'FAIL ' + str(err)}", flush=True)
        return (ok, f"connect-failed:{err_class(exc)}")

    t_connect = time.time() - t_connect_start
    t_first, t_total, status, resp_len, err = timed_request(
        s, send_bytes, timeout=timeout)

    if err:
        print(f"    t_connect={t_connect:.3f}s t_first=None t_total={t_total:.3f}s "
              f"status=None len={resp_len} err={err}", flush=True)
    else:
        print(f"    t_connect={t_connect:.3f}s t_first={t_first:.3f}s "
              f"t_total={t_total:.3f}s status={status} len={resp_len}", flush=True)

    try:
        s.close()
    except OSError:
        pass

    # Контрольный GET после каждого теста
    ok, err = control_get(host, port)
    if ok:
        print(f"    CONTROL GET: OK (server recovered)", flush=True)
        return (True, f"status={status} err={err} control=OK")
    else:
        print(f"    CONTROL GET: FAIL ({err}) — SERVER HUNG!", flush=True)
        return (False, f"status={status} err={err} control=FAIL:{err}")


def main():
    ap = argparse.ArgumentParser(description="HTTPD security stress test")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--quick", action="store_true",
                    help="Быстрый режим (меньше размеров и таймаутов)")
    args = ap.parse_args()

    host = args.host
    port = args.port

    if args.quick:
        sizes = [128, 512]
        header_sizes = [256, 512]
        body_sizes = [256, 512]
    else:
        sizes = [1024, 4096, 16384]
        header_sizes = [1024, 4096]
        body_sizes = [1024, 4096, 16384]

    print(f"Target: {host}:{port}")
    print(f"Quick mode: {args.quick}")
    print()

    failed = 0
    total = 0

    # === Группа 1: Переполнение request line ===
    print("=== Группа 1: Переполнение request line ===")
    for size in sizes:
        total += 1
        # Request line без CRLF — просто огромная строка
        send_data = b"A" * size
        ok, details = run_case(
            f"Request line {size} байт без CRLF",
            send_data, host, port)
        if not ok:
            failed += 1

    # Request line с огромным URI
    for size in sizes:
        total += 1
        send_data = b"GET /" + b"A" * size + b" HTTP/1.0\r\n\r\n"
        ok, details = run_case(
            f"URI {size} байт с CRLF",
            send_data, host, port)
        if not ok:
            failed += 1

    # === Группа 2: Огромные заголовки ===
    print("\n=== Группа 2: Огромные заголовки ===")
    for size in header_sizes:
        total += 1
        send_data = (b"GET / HTTP/1.0\r\n" +
                     b"X-Large-Header: " + b"A" * size + b"\r\n\r\n")
        ok, details = run_case(
            f"Один заголовок {size} байт",
            send_data, host, port)
        if not ok:
            failed += 1

    # Множество заголовков
    total += 1
    many_headers = b"".join(b"X-Header-%d: %s\r\n" % (i, b"x" * 64) for i in range(50))
    send_data = b"GET / HTTP/1.0\r\n" + many_headers + b"\r\n"
    ok, details = run_case(
        "50 заголовков по 64 байта",
        send_data, host, port)
    if not ok:
        failed += 1

    # Повторяющиеся заголовки
    total += 1
    send_data = b"GET / HTTP/1.0\r\n" + b"X-Dup: x\r\n" * 100 + b"\r\n"
    ok, details = run_case(
        "100 повторяющихся заголовков",
        send_data, host, port)
    if not ok:
        failed += 1

    # === Группа 3: Content-Length атаки ===
    print("\n=== Группа 3: Content-Length ===")
    total += 1
    send_data = b"POST / HTTP/1.0\r\nContent-Length: 999999999\r\n\r\n"
    ok, details = run_case(
        "Content-Length: 999999999 (без тела)",
        send_data, host, port, timeout=3.0)
    if not ok:
        failed += 1

    total += 1
    send_data = b"POST / HTTP/1.0\r\nContent-Length: -1\r\n\r\n"
    ok, details = run_case(
        "Content-Length: -1",
        send_data, host, port, timeout=3.0)
    if not ok:
        failed += 1

    total += 1
    send_data = b"POST / HTTP/1.0\r\nContent-Length: abc\r\n\r\n"
    ok, details = run_case(
        "Content-Length: abc (не число)",
        send_data, host, port, timeout=3.0)
    if not ok:
        failed += 1

    # === Группа 4: Неполные/повреждённые запросы ===
    print("\n=== Группа 4: Неполные и повреждённые запросы ===")
    total += 1
    send_data = b"GET /"  # Без CRLF, без тела
    ok, details = run_case(
        "GET / без CRLF",
        send_data, host, port, timeout=4.0)
    if not ok:
        failed += 1

    total += 1
    send_data = b"\r\n\r\n"  # Пустой заголовок
    ok, details = run_case(
        "Только \\r\\n\\r\\n",
        send_data, host, port)
    if not ok:
        failed += 1

    total += 1
    send_data = b"GET /\r\n"  # Незавершённые заголовки (нет пустой строки)
    ok, details = run_case(
        "GET /\\r\\n без окончания заголовков",
        send_data, host, port, timeout=4.0)
    if not ok:
        failed += 1

    total += 1
    send_data = b"\x00\x01\x02\xff\xfe\xfdGET / HTTP/1.0\r\n\r\n"  # Бинарный мусор
    ok, details = run_case(
        "Бинарный мусор перед запросом",
        send_data, host, port)
    if not ok:
        failed += 1

    total += 1
    send_data = b"\x16\x03\x01\x00\x40" + b"\x00" * 64  # TLS ClientHello
    ok, details = run_case(
        "TLS ClientHello (~80 байт)",
        send_data, host, port)
    if not ok:
        failed += 1

    # === Группа 5: Медленный клиент ===
    print("\n=== Группа 5: Медленный клиент ===")
    total += 1
    print("  TEST: 1 байт в секунду (5 байт, 5 пауз)")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(CONNECT_TIMEOUT)
    try:
        s.connect((host, port))
        t_connect = time.time() - time.time()  # 0, уже подключились
    except OSError as exc:
        print(f"    CONNECT FAILED: {err_class(exc)}")
        try:
            s.close()
        except OSError:
            pass
        ok, err = control_get(host, port)
        if not ok:
            failed += 1
        total += 0  # Уже учтено
        print()
        print(f"РЕЗУЛЬТАТ: {failed} из {total} тестов провалены")
        return 1 if failed else 0

    t_connect_start = time.time()
    s.connect((host, port))
    t_connect = time.time() - t_connect_start

    try:
        for i in range(5):
            s.send(bytes([ord("G"), ord("E"), ord("T"), ord(" "), ord("/")][i:i+1]))
            time.sleep(1.0)
        # Даём серверу время обработать (или не обработать)
        s.settimeout(RECV_TIMEOUT)
        data = b""
        try:
            while True:
                chunk = s.recv(RECV_CHUNK)
                if not chunk:
                    break
                data += chunk
                if b"\r\n\r\n" in data:
                    break
        except socket.timeout:
            pass
        t_total = time.time() - t_connect_start
        if data.startswith(b"HTTP/"):
            print(f"    t_connect={t_connect:.3f}s t_total={t_total:.3f}s "
                  f"status=200 len={len(data)} — сервер дождался полного запроса")
        else:
            print(f"    t_connect={t_connect:.3f}s t_total={t_total:.3f}s "
                  f"ответ={data[:50]!r} — сервер не дождался или вернул ошибку")
    except OSError as exc:
        print(f"    Ошибка при отправке: {err_class(exc)}")
    finally:
        try:
            s.close()
        except OSError:
            pass

    ok, err = control_get(host, port)
    if ok:
        print(f"    CONTROL GET: OK (server recovered after slow client)", flush=True)
    else:
        print(f"    CONTROL GET: FAIL ({err}) — SERVER HUNG!", flush=True)
        failed += 1

    # === Группа 6: Закрытие соединения в разные моменты ===
    print("\n=== Группа 6: Раннее закрытие соединения ===")
    total += 1
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(CONNECT_TIMEOUT)
    try:
        s.connect((host, port))
        s.sendall(b"GET /cgi-bin/hello HTTP/1.0\r\nHost: x\r\n")
        # Закрываем без отправки завершающего \r\n\r\n
        s.close()
        print(f"    Отправили часть заголовка, закрыли без конца — сервер должен сбросить слот")
    except OSError as exc:
        print(f"    Ошибка: {err_class(exc)}")
    ok, err = control_get(host, port)
    if ok:
        print(f"    CONTROL GET: OK (server recovered)", flush=True)
    else:
        print(f"    CONTROL GET: FAIL ({err}) — SERVER HUNG!", flush=True)
        failed += 1

    print()
    print(f"РЕЗУЛЬТАТ: {failed} из {total} тестов провалены")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
