# M1 CANopen / NUCLEO-G474RE

The selected motor is the user-specified TECO DTSX06401C4NT3D01, DC48V 400W,
maximum **4000 motor rpm**. This is not a default motion command or wheel rpm.

See the [AGX / STM32 integration plan](docs/stm32-differential-ugv-integration.md)
for the architecture, vehicle geometry, protocol choices and remaining work.

**The application now runs read-only diagnostics.** See the
[build, flashing, console and C2 health guide](docs/read-only-diagnostics.md).
Power-on is passive; `WATCH ON` starts polling and `PROBE` reads identity/config.
There are no activation, motor target, parameter write, NMT or SYNC commands.
The AGX monitor emits fresh per-drive health plus a normal/error UI mapping;
the existing C2 WebSocket/driver integration is still separate work.
Diagnostic commands support correlated `REQ <id> <command>` ACKs. The AGX
monitor verifies each configuration reply and asynchronous probe completion;
missing/rejected replies cannot be masked by healthy telemetry. See the
[newarchitecture ACK audit](docs/m1-stm32-ack-contract.md).

`Core/Inc/m1_canopen.h` and `Core/Src/m1_canopen.c` implement a portable C99
protocol codec based on the supplied DEXMART CANopen Rev.8 (2026-01-15):

- Expedited SDO read/write with node/index/subindex correlation and abort decoding.
- CiA402 CAN RPDO1/2/3 and TPDO1/2 codecs; exact mapping comparison.
- CiA402 status decoding, including remote PV readiness (not freshness).
- Explicit, verified raw-units-per-motor-rpm conversion and a 4000 rpm ceiling.
- Differential kinematics with independent wheel radii, gear ratios and signs;
  proportional saturation retains turning curvature.

The codec/kinematics library is accompanied by `m1_diagnostics.c` (portable
read-only scheduler/health logic) and `m1_diag_board.c` (G474RE HAL/USB VCP binding).
`main.c` calls the diagnostic runtime. Both the new GCC build and EWARM project
include its sources. This is not a finished motion controller.

## Protocol profile to verify before motion

Rev.8 contains several product/firmware profiles. The proposed implementation
uses **Control Mode 5 (CiA402 CAN)** and **6060 = 3 (Profile Velocity)**, subject
to readback on the actual M1 firmware. Do not interpret MultiCMD PDOs as CiA402,
or copy the EtherCAT layout into an 8-byte Classic CAN frame.

The expected maps (PDF pp.101-102) are:

| PDO | Standard CAN ID | Layout |
| --- | --- | --- |
| RPDO1 | 0x200 + node | controlword u16, mode i8, target position i32 (7 bytes) |
| RPDO2 | 0x300 + node | target velocity i32, profile velocity u32 (8 bytes) |
| RPDO3 | 0x400 + node | acceleration u32, deceleration u32 (8 bytes) |
| TPDO1 | 0x180 + node | statusword u16, mode display i8, actual position i32 (7 bytes) |
| TPDO2 | 0x280 + node | actual velocity i32, digital inputs u32 (8 bytes) |

All multibyte values are little endian. HAL binding must reject extended IDs,
RTR and CAN FD frames before handing them to this standard-frame codec.
Read back PDO counts/entries, COB-IDs, trigger types, control mode, units and
firmware before using the matching codec. The codec alone does not gate this.
RPDO3 mapping is 60830020,60840020; mapping checking accepts caller-supplied maps.

Target velocity is a **user-defined unit**, not guaranteed rpm (PDF p.137).
Factor Group and SI metadata are described on pp.109-113. The example mm/s
encoding is not proof of the actual configuration. `M1_VelocityScale.verified`
may only be set after confirming settings and measured direction/speed.
No function automatically changes factors, EEPROM, polarity or motor parameters.

## Remaining runtime work

Read-only SDO scheduling, segmented identity strings, timestamp/freshness,
CAN error/EMCY/heartbeat health and a diagnostic console are implemented.
Remaining motion work includes verified startup admission, the both-wheel
enable/stop state machine, independent motion-command watchdog, drive-side
communication loss behavior, and the versioned AGX/STM32 motion serial link.
The AGX driver must preserve the existing locomotion SI interface and motion
ownership. Do not report CAN submission as physical standstill.

The supplied nominal geometry is wheel diameter **440 mm**, left/right wheel
center distance **739 mm**, and reduction **10:1** (motor turns per wheel turn).
Thus use `track_m=0.739`, both radii `0.220`, and both gear ratios `10.0`.
Installation signs, effective wheel radius/turn calibration, motor/encoder
matching, braking, drive firmware and unit scale remain unverified. Keep activation unavailable until these are
validated. Reboot/fault recovery must clear previous nonzero targets and require
explicit reactivation. Raw RPDO builders pack values; they do not enforce the
motor rpm limit or replace the controller's motion admission checks.

## Host tests

```sh
cmake -S tests -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

Tests cover exact payloads, negative values, SDO correlation/abort/malformed
responses, incompatible mappings, statusword masks, verified units, maximum rpm,
geometry errors, both turning directions, installation signs and joint saturation.
Tests include both synthetic edge cases and the supplied vehicle geometry.
No test marks the physical vehicle as calibrated or enables the application.

For the complete diagnostic image on Windows:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_diagnostics.ps1
```

This produces ELF/HEX/BIN in `build/diagnostics`. No flashing is performed.
The GCC build is verified; hardware tests and an IAR build remain outstanding.
