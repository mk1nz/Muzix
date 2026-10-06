# Zeta SBC V2 512 KiB Flash ROM layout

`make rom` creates `build/muzix-zeta-v2.rom`, exactly 524,288 bytes.

| ROM range | Purpose |
| --- | --- |
| `0x00000..0x00097` | reset stub (`build/muzix-rom-boot.bin`): copy pages 0..3 to RAM pages 32..35, then `jp 0x0098` |
| `0x00098..0x1ffff` | reserved 128 KiB kernel-image region; entry begins at `0x0098` |
| `0x20000..0x7ffff` | Filesystem using the MINIX-v1 format (`build/muzix-minifs.bin`), 384 KiB |

`tools/make_zeta_rom.sh` assembles it: the boot stub at 0, the kernel entry at
`stage2_offset=152` (`0x0098`), and the filesystem at `0x20000`. The image
reserves 128 KiB for the kernel region, while the reset stub initially copies
four 16 KiB pages into RAM. The remaining ROM space is not allocator-owned RAM;
user pages are allocated from physical RAM through MM.

## The filesystem image uses the MINIX-v1 format

This image uses the MINIX-v1 filesystem format (magic `0x137f`, 512-byte
blocks), built by
`tools/make_minixfs.rb` from `build/<app>.bin` and three documents. `make rom`
places it at `0x20000`; the kernel mounts it through
`kernel/rom_read.c`, and the shell execs programs out of its root by bare
name.

> An earlier version of this file said the image carried an "MZRFS staging
> archive" at `0x10000`, 448 KB, holding `init`, `echo`, `cat`, `ls` and the
> shell, and that a freestanding kernel linker and a filesystem loader were
> "still required before these programs can be executed with `exec`". All of
> that described an earlier state of the tree. `make_romfs.rb`, which wrote
> MZRFS1, still exists and has been moved to `oldOrphanedCodeBackup/`; nothing
> in the build referenced it even before that. The image is assembled from the
> MINIX-v1-format filesystem by `make_zeta_rom.sh`.

It carries twelve files, all flat — the only directory entries are `.` and `..`:

| file | what it is |
| --- | --- |
| `shell` | PID 1 |
| `echo` | user command |
| `cat` | user command |
| `ls` | user command |
| `date` | reads and sets the DS1302 |
| `top` | process table and screen-clearing program |
| `uptime` | interval since the boot reading of the clock |
| `loadavg` | kernel CPU busy-fraction windows |
| `hwdiag` | checks for userspace-visible RTC, load accounting, processes, ROMFS, and console query |
| `readme` | `docs/architecture.md` |
| `hw-notes` | `docs/hardware-notes.md` |
| `copy-notes` | `docs/memory-copy-notes.md` |

`hwdiag` reports the PPI, floppy controller, UART loopback and SRAM/flash page
integrity as not tested: Muzix does not currently expose safe userspace drivers
or diagnostic interfaces for them. Board components listed here follow the
[Zeta SBC V2 hardware documentation](https://github.com/skiselev/zeta_sbc/blob/0f57a102bc2ce50e34235c788376c5e989405907/README.md).

The exact used-byte count is reported by the ROM build; `tools/rom_validator.rb`
fails the build below 1 KiB free.

There is no `init` in the image and never was. The shell is PID 1;
`kernel/kernel_main.c` execs `"shell"` into slot 1.

Use `make test-rom-image` to check the size and the mandatory offsets, and
`make test-rom-validator` to check the filesystem's structure and all twelve
files.
