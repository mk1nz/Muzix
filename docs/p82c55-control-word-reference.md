# КР580ВВ55А / 8255A Control Word Reference

## Overview

The КР580ВВ55А (also known as КР580ВВ55А, i8255A, КР580ИВ55А) is a Soviet
clone of the Intel 8255A Programmable Peripheral Interface (PPI). It provides
three 8-bit ports (A, B, C) and a control register, with three operating modes.

## Pinout and Port Assignment

### Ports (addresses A1=0, A0=0, A1=0, A0=1, A1=1, A0=0, A1=1, A0=1)

| A1 | A0 | Selected Register |
|----|----|-------------------|
| 0  | 0  | Port A |
| 0  | 1  | Port B |
| 1  | 0  | Port C |
| 1  | 1  | Control/Status Register |

### Port C Split

Port C is split into two 4-bit halves:
- **Port C Upper (C7–C4)**: Grouped with Port A
- **Port C Lower (C3–C0)**: Grouped with Port B

## Control Word Format

Two types of control words can be written:

### 1. Mode Set Word (D7 = 1)

When D7 = 1, the word is a mode-setting control word:

```
D7 D6 D5 D4 D3 D2 D1 D0
 |  |  |  |  |  |  |  |
 1 Mode  PA  PCU  PB  PCL-dir
```

Bit assignments (Mode 0, the simple I/O mode used in this project):

| Bit | Name | Description | Values |
|-----|------|-------------|--------|
| D7  | M    | Mode set flag | 1 = mode set (not BSR) |
| D6  | —    | Group A mode | 00 = Mode 0 |
| D5  | —    | Group A mode | 00 = Mode 0 |
| D4  | DIRA | Port A direction | 1 = Input, 0 = Output |
| D3  | DIRC | Port C upper (C7–C4) direction | 1 = Input, 0 = Output |
| D2  | MODE | Group B mode | 0 = Mode 0 (standard I/O) |
| D1  | DIRB | Port B direction | 1 = Input, 0 = Output |
| D0  | DIRL | Port C lower (C3–C0) direction | 1 = Input, 0 = Output |

**Key bit: D0 controls Port C lower (C0–C3) direction**:
- D0 = 1 → Port C lower = **Input**
- D0 = 0 → Port C lower = **Output**

### 2. Bit Set/Reset (BSR) Word (D7 = 0)

When D7 = 0, the word sets or resets an individual bit of Port C:

```
D7 D6 D5 D4 D3 D2 D1 D0
 0  —  —  —  —  — Bit Set/Reset
```

| Bit | Name | Description |
|-----|------|-------------|
| D7  | —    | 0 = BSR mode |
| D6–D3 | —  | Don't care |
| D2–D1 | Bit | Bit number (0–7) |
| D0  | S/R | 1 = Set, 0 = Reset |

## Mode 0 Direction Truth Table

The following table (from dplm2008.narod.ru and hromatron.narod.ru lab reports)
shows all 16 combinations for Mode 0:

| D4 | D3 | D2 | D1 | D0 | Port A | C[7:4] | Port B | C[3:0] |
|----|----|----|----|----|--------|--------|--------|--------|
| 0  | 0  | 0  | 0  | 0  | Output | Output | Output | Output |
| 0  | 0  | 0  | 0  | 1  | Output | Output | Output | Input  |
| 0  | 0  | 0  | 1  | 0  | Output | Output | Input  | Output |
| 0  | 0  | 0  | 1  | 1  | Output | Output | Input  | Input  |
| 0  | 1  | 0  | 0  | 0  | Output | Input  | Output | Output |
| 0  | 1  | 0  | 0  | 1  | Output | Input  | Output | Input  |
| 0  | 1  | 0  | 1  | 0  | Output | Input  | Input  | Output |
| 0  | 1  | 0  | 1  | 1  | Output | Input  | Input  | Input  |
| 1  | 0  | 0  | 0  | 0  | Input  | Output | Output | Output |
| 1  | 0  | 0  | 0  | 1  | Input  | Output | Output | Input  |
| 1  | 0  | 0  | 1  | 0  | Input  | Output | Input  | Output |
| 1  | 0  | 0  | 1  | 1  | Input  | Output | Input  | Input  |
| 1  | 1  | 0  | 0  | 0  | Input  | Input  | Output | Output |
| 1  | 1  | 0  | 0  | 1  | Input  | Input  | Output | Input  |
| 1  | 1  | 0  | 1  | 0  | Input  | Input  | Input  | Output |
| 1  | 1  | 0  | 1  | 1  | Input  | Input  | Input  | Input  |

**Note**: D2 = 0 in all Mode 0 entries above. D2 = 1 would select Mode 1
(strobe I/O), which is not used in this simple I/O application.

## Zeta SBC V2 / Muzix I2C Configuration

### Hardware Wiring

On the Zeta SBC V2, the КР580ВВ55А is at I/O ports 0x60–0x63:

| Port   | Address | Usage in Muzix |
|--------|---------|----------------|
| Port A | 0x60    | Unused |
| Port B | 0x61    | Unused |
| Port C | 0x62    | PC0 = SDA (I2C data, lower nibble), PC4 = SCL (I2C clock, upper nibble) |
| Control| 0x63    | Mode/direction control |

**Key design choice**: SCL is on PC4 (upper nibble), SDA on PC0 (lower nibble).
This separates them into different nibbles so D0 (Port C lower direction) can
switch SDA between OUTPUT/INPUT for ACK sampling while D3 (Port C upper direction)
keeps SCL as OUTPUT. If SCL were on PC1 (same lower nibble as SDA), switching
PC0 to INPUT would also release SCL, making clock stretching unpredictable.

### Control Words for I2C

- **All output mode** (PC0=SDA output, PC4=SCL output):
  - `CTL_OUTPUT = 0x90` = `1001 0000`
    - D7=1 (mode set), D6-D5=00 (Mode 0), D4=0 (PA out),
      D3=0 (PC upper out), D2=0 (Mode 0), D1=0 (PB out), D0=0 (PC lower out)

- **Port C lower input** (release SDA for ACK reading, keep SCL output):
  - `CTL_C_LOWER_INPUT = 0x91` = `1001 0001`
    - D7=1 (mode set), D6-D5=00 (Mode 0), D4=0 (PA out),
      D3=0 (PC upper out), D2=0 (Mode 0), D1=0 (PB out), D0=1 (PC lower in)

### I2C Operation

1. Write `CTL_OUTPUT` (0x90) to set PC0/SDA and PC4/SCL as outputs.
2. Drive SDA and SCL via writes to Port C (0x62).
3. After sending a byte, switch to `CTL_C_LOWER_INPUT` (0x91) to release SDA (PC0)
   for ACK reading while keeping SCL (PC4) as OUTPUT.
4. Read Port C: check if the slave pulled SDA (PC0) low (ACK).
5. Switch back to OUTPUT to continue or send STOP.

## References

1. **Intel 8255A Datasheet** — Standard reference for all control word formats.
2. **computer-museum.ru**: https://www.computer-museum.ru/technlgy/i8255.htm
3. **dplm2008.narod.ru**: https://dplm2008.narod.ru/str/komplects/kr580/kr580vv55a.html
   — Contains the definitive truth table for Mode 0 direction settings.
4. **hromatron.narod.ru (lab report 1)**:
   https://hromatron.narod.ru/labi/izuchenie-parallelnogo-interfeysa-na-kr580vv55.htm
   — Laboratory exercise demonstrating control word programming.
5. **hromatron.narod.ru (lab report 2)**:
   https://hromatron.narod.ru/labi/izuchenie-adaptera-parallelnogo-obmena-na-kr580vv55a.htm
   — Adapter programming exercise with control word examples (e.g., CW=0x9D).
6. **zxdn.narod.ru**:
   https://zxdn.narod.ru/hardware/dv0akemp.htm
   — Kempston Mouse Interface using КР580ВВ55А, demonstrating practical
     use of all three ports and control word programming.
7. **studfile.net**:
   https://studfile.net/preview/9129050/page:20/
   — Educational material on ППИ ВВ55, including control word format
     (Fig 2.14а) and port addressing (Table 2.12).
