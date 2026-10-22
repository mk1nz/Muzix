#!/usr/bin/env python3
"""Compute Unix-like epochs for test dates (Jan 1 2000 = 0)."""

def epoch_for(year, month, day, hour=0, minute=0, second=0):
    days = 0
    for y in range(2000, year):
        leap = (y % 4 == 0 and y % 100 != 0) or (y % 400 == 0)
        days += 366 if leap else 365
    months = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    leap = (year % 4 == 0 and year % 100 != 0) or (year % 400 == 0)
    if leap:
        months[1] = 29
    for m in range(month - 1):
        days += months[m]
    days += (day - 1)
    return days * 86400 + hour * 3600 + minute * 60 + second

tests = [
    (2000, 1, 1, 0, 0, 0, "01.01.2000 00:00:00"),
    (2000, 2, 29, 0, 0, 0, "29.02.2000 00:00:00"),
    (2000, 3, 1, 0, 0, 0, "01.03.2000 00:00:00"),
    (2001, 1, 1, 0, 0, 0, "01.01.2001 00:00:00"),
    (2001, 2, 28, 0, 0, 0, "28.02.2001 00:00:00"),
    (2001, 3, 1, 0, 0, 0, "01.03.2001 00:00:00"),
    (2000, 2, 29, 12, 34, 56, "29.02.2000 12:34:56"),
    (2004, 2, 29, 12, 30, 45, "29.02.2004 12:30:45"),
    (2100, 2, 28, 0, 0, 0, "28.02.2100 00:00:00"),
    (2100, 3, 1, 0, 0, 0, "01.03.2100 00:00:00"),
    (2000, 12, 31, 23, 59, 59, "31.12.2000 23:59:59"),
    (2003, 1, 1, 0, 0, 0, "01.01.2003 00:00:00"),
]

for y, m, d, h, mi, s, expected in tests:
    e = epoch_for(y, m, d, h, mi, s)
    print(f"epoch={e:10d} (0x{e:08X}) -> expected=\"{expected}\"")
