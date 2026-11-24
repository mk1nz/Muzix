#!/usr/bin/env python3
"""Decode I2C trace output from z80pack-emu verbose mode."""
import sys
import re

def decode_trace(trace_file):
    bits_by_byte = []
    current_bits = []
    bytes_sent = []
    byte_count = 0

    with open(trace_file, 'r') as f:
        for line in f:
            # Match "Sampled bit N: sda=X (data now 0xNN)"
            m = re.search(r'Sampled bit \d+: sda=(\d) \(data now 0x([0-9A-Fa-f]+)\)', line)
            if m:
                bit_val = int(m.group(1))
                current_bits.append(bit_val)

            # Match "Byte complete: 0xNN"
            m = re.search(r'Byte complete: 0x([0-9A-Fa-f]+)', line)
            if m:
                byte_val = int(m.group(1), 16)
                bytes_sent.append((byte_count, byte_val, list(current_bits)))
                byte_count += 1
                current_bits = []

    print("=== I2C Data Bytes ===")
    for bc, val, bits in bytes_sent:
        label = "ADDR" if bc == 0 else f"DATA[{bc-1}]"
        print(f"  {label}: 0x{val:02X} = 0b{val:08b}  bits: {bits}")

if __name__ == '__main__':
    if len(sys.argv) > 1:
        decode_trace(sys.argv[1])
    else:
        decode_trace('/dev/stdin')
