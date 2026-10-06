# Zeta V2 test models

This directory contains standalone bank-model, context-switch, and MINIX
adapter tests. These are test doubles for controlled memory-map checks; they
are not the production scheduler, process map, or user-copy path and are not
linked into the ROM kernel.

The production path uses MM-owned page allocation and copy operations, with
bank mapping implemented in `platform/zeta-v2/context_switch.c`. In the active
userspace map, window 0 and window 3 retain kernel pages while process pages
occupy windows 1 and 2. Temporary mappings restore the complete kernel map.

The code here remains useful for testing invariants that can be represented by
the model. It does not establish that the production assembly or real hardware
behaves correctly; ROM and emulator validation are separate.
