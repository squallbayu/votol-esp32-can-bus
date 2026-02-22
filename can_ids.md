# Votol CAN IDs (as documented in this repo)

## Scope
This document lists the CAN IDs and fields that are present in this repository's code and notes. At the moment, only two standard 11-bit CAN IDs are documented here:

- 0x3FF (1023) request / poll
- 0x3FE (1022) response / live data

If your controller uses additional IDs, they are not yet captured in this repo.

## Conventions
- Standard 11-bit CAN IDs.
- Each CAN frame is 8 bytes.
- The 0x3FE response is treated as a 24-byte stream B0..B23 built from three frames:
  - B0..B7  = frame0[0..7]
  - B8..B15 = frame1[0..7]
  - B16..B23 = frame2[0..7]

This means multi-byte values can cross frame boundaries (for example, B7/B8).

## ID 0x3FF (1023) - request / poll
Direction: display -> controller

Two frames are sent back-to-back. The firmware in `embassy/src/bin/can/can_communication.rs` sends them every 300 ms.

- Frame A payload:
  `09 55 AA AA 00 AA 00 00`
- Frame B payload:
  `00 18 AA 05 D2 00 20 33`

Notes:
- These bytes are copied from an Endless Sphere thread and are not fully validated across controllers.

## ID 0x3FE (1022) - response / live data
Direction: controller -> display

A single update is 3 frames. The layout below is based on `embassy/src/bin/can/can_frame.rs`, `embassy/src/bin/can/can_communication.rs`, and `reference/raw_notes.md`.

### Frame layout (byte-level)

Frame 0 (0x3FE):

- byte0: 0x09 (const)
- byte1: 0x55 (const)
- byte2: 0xAA (const)
- byte3: 0xAA (const)
- byte4: 0x00 (unknown/reserved in examples)
- byte5: 0x00 (unknown/reserved in examples)
- byte6: 0x00 (unknown/reserved in examples)
- byte7: BV_H (battery voltage high byte)

Frame 1 (0x3FE):

- byte0: BV_L (battery voltage low byte)
- byte1: BC_H (battery current high byte)
- byte2: BC_L (battery current low byte)
- byte3: 0x00 (unknown/reserved in examples)
- byte4: ERR_1 (error code byte 1, MSB)
- byte5: ERR_2 (error code byte 2)
- byte6: ERR_3 (error code byte 3)
- byte7: ERR_4 (error code byte 4, LSB)

Frame 2 (0x3FE):

- byte0: RPM_H
- byte1: RPM_L
- byte2: CT (controller temperature + 50)
- byte3: ET (external/motor temperature + 50)
- byte4: STATUS_A (unknown)
- byte5: STATUS_B (unknown)
- byte6: STATUS_C (unknown; often 0x01 in examples)
- byte7: STATE (controller state code)

### Field decoding
All references below use the B0..B23 indexing described in Conventions.

- Battery voltage (V): `((B7 << 8) | B8) / 10`
- Battery current (A): `((B9 << 8) | B10) / 10` (signed 16-bit)
- RPM: `(B16 << 8) | B17` (signed 16-bit)
- Controller temp (C): `B18 - 50`
- External/motor temp (C): `B19 - 50`
- Controller state: `B23` (see mapping below)
- Error code (u32, big-endian): `(B12 << 24) | (B13 << 16) | (B14 << 8) | B15`

### Controller state codes

- 0: IDLE
- 1: INIT
- 2: START
- 3: RUN
- 4: STOP
- 5: BRAKE
- 6: WAIT
- 7: FAULT

### Error bit meanings
The firmware currently returns the first matching error bit by lowest bit order. Multiple errors can be set at once.

| Mask        | Name                 | Meaning (per code comments)              |
|-------------|----------------------|------------------------------------------|
| 0x00000001  | EBrakeOn             | Brake                                    |
| 0x00000002  | OverCurrent          | Hardware overcurrent                     |
| 0x00000004  | UnderVoltage         | Under voltage                            |
| 0x00000008  | ThrottleHallError    | Throttle Hall error                      |
| 0x00000010  | OverVoltage          | Over voltage                             |
| 0x00000020  | McuError             | Controller error                         |
| 0x00000040  | MotorBlock           | Motor block error                        |
| 0x00000080  | FootplateErr         | Throttle error                           |
| 0x00000100  | SpeedControl         | Run away                                 |
| 0x00000200  | WritingEeprom        | EEPROM writing                           |
| 0x00000800  | StartUpFailure       | Quality inspection failure               |
| 0x00001000  | Overheat             | Controller overheat                      |
| 0x00002000  | OverCurrent1         | Software overcurrent                     |
| 0x00004000  | AcceleratePadalErr   | Throttle failure                         |
| 0x00008000  | Ics1Err              | Current sensor error 1                   |
| 0x00010000  | Ics2Err              | Current sensor error 2                   |
| 0x00020000  | BreakErr             | Brake failure                            |
| 0x00040000  | HallSelError         | Hall error                               |
| 0x00080000  | MosfetDriverFault    | Driver failure                           |
| 0x00100000  | MosfetHighShort      | MOS tube short circuit                   |
| 0x00200000  | PhaseOpen            | Phase wire connection failure            |
| 0x00400000  | PhaseShort           | Phase wire short circuit                 |
| 0x00800000  | McuChipError         | Controller failure                       |
| 0x01000000  | PreChargeError       | Pre-charge failure                       |
| 0x08000000  | MotorOverheat        | Motor overheat                           |
| 0x80000000  | SocZeroError         | SOC 0 error                              |

### Status bits (unverified)
`reference/raw_notes.md` describes a status/gear byte (called B22 there) with this bitfield:

- bits 0-1: 0=L, 1=M, 2=H, 3=S
- bit 2: R
- bit 3: P
- bit 4: brake
- bit 5: antitheft
- bit 6: side stand
- bit 7: regen

The exact byte index of this status field in the CAN response is not confirmed in code yet. If the B22 naming is literal (B0..B23), it would correspond to frame2[6].

## Example 0x3FE response frames
From `reference/raw_notes.md`:

- frame0: `09 55 AA AA 00 00 00 01`
- frame1: `27 00 01 00 00 00 00 84`
- frame2: `00 00 4A F0 00 00 01 07`

These frames show the constant header, battery voltage/current fields, error code 0x00000084, temperatures with +50 offset, and state 0x07 (FAULT).
