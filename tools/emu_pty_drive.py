#!/usr/bin/env python3
"""Drive the Muzix emulator over a pty and print everything it sends back.

Piping stdin is not the interactive case: the TTY canonical path and the UART
echo differ, so a piped run says nothing about what a user at a terminal gets.
This writes to a real pty and echoes everything the emulator prints, so a
command that never arrived is visible as the absence of its own echo.

Each command is sent when the shell prompt appears, so the input lands in the
terminal driver's canonical path the way a person typing would land it.

Known limit of the emulator, not of the kernel: over a pty the stdin bridge
hands the UART only the first word of a line, so `cat readme` arrives as `cat`.
The kernel's own TTY echo proves it - those bytes never reach it - and the same
ROM over a pipe takes the whole line. Use one word per pty run and check the
echo; use a pipe for anything with a space.
"""
import os
import pty
import select
import sys
import time

ROM = os.environ.get("EMU_ROM", "build/muzix-zeta-v2.rom")
EMU = "./emulator/z80pack-emu"
CYCLES = os.environ.get("EMU_CYCLES", "600000000")
PROMPT = b"/> "
SETTLE = float(os.environ.get("EMU_SETTLE", "30"))

cmds = sys.argv[1:] or ["help"]
argv = [EMU, "-s", "muzix", ROM, "-c", CYCLES, "-i"]

pid, fd = pty.fork()
if pid == 0:
    os.execv(argv[0], argv)

out = bytearray()
deadline = time.time() + 240
sent = 0
last = time.time()

while time.time() < deadline:
    r, _, _ = select.select([fd], [], [], 0.2)
    if fd in r:
        try:
            data = os.read(fd, 4096)
        except OSError:
            break
        if not data:
            break
        out.extend(data)

    if sent < len(cmds):
        # The prompt is the last thing the shell prints before it reads, so
        # send when the next one is on screen.
        if out.count(PROMPT) >= sent + 1 and time.time() - last > 2.0:
            os.write(fd, (cmds[sent] + "\n").encode())
            sent += 1
            last = time.time()
    elif time.time() - last > SETTLE:
        break

try:
    os.close(fd)
except OSError:
    pass
try:
    os.kill(pid, 9)
    os.waitpid(pid, 0)
except (ProcessLookupError, ChildProcessError):
    pass

sys.stdout.write(out.decode("utf-8", "replace"))
