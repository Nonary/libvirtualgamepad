# DualSense USB compatibility

The Windows VHF profile uses our portable `ds5_usb.h` contract. It has no
ViGEm dependency. Linux UHID DualSense uses the same firmware UpdateVersion,
USB descriptor, and sensor-enable command via Inputtino's `ds5_usb.hpp`. HD
haptics still need a DualSense USB audio function, which neither VHF nor UHID
provides.

Resonance: A Plague Tale Legacy identifies Sony 054c:0ce6, then classifies the
transport and initializes native scePad input. Matching VID/PID and sending
readable button reports does not satisfy that sequence.

## Verified native requirements

| Operation | USB requirement | Original defect |
| --- | --- | --- |
| Transport classification | Feature 85, page FF00, usage 2D, count 2 | Missing; classified as Bluetooth or skipped |
| Firmware descriptor | Feature 20, page FF00, usage 26, count 63 | Already correct |
| Firmware/calibration agreement | LE32 at offset 28 >= 0001003E selects calibration usage 33 | 00000100 selected legacy usage 23, absent from our descriptor |
| Firmware UpdateVersion | LE16 at offset 44 >= 0390 | 0000 made libScePad report a required DualSense firmware update and skip triggers/rumble |
| Improved rumble | Output valid_flag2 bit 2 (COMPATIBLE_VIBRATION2) | Only valid_flag0 bit 0 was decoded, so libScePad rumble v2 was ignored |
| Bluetooth output | Report 0x31, 78 bytes, common payload at offset 3 | USB-only 0x02 decode; libScePad sends 0x31 when OutputReportByteLength > 48 |
| Calibration descriptor | Feature 05, page FF00, usage 33, count 40 | Correct for the modern USB revision |
| Sensor initialization | SetFeature 08, 48 bytes, command byte 02, reserved bytes zero | Descriptor omitted and command rejected |
| Input / output / maximum feature sizes | 64 / 48 / 64 bytes including report ID | Already correct |
| Pairing descriptor | Feature 09, page FF00, usage 24, count 19 | Incorrect usage 34 |

The corrected descriptor exposes the complete USB feature inventory and restores
logical maximum 255 after the button fields. We implement the native sensor-enable
command precisely; unsupported factory/authentication operations still fail.
Firmware fields describe our supported protocol revision, not a claimed factory
build or driver-package version. Build strings and optional feature version stay
empty. Native transport classification depends on the descriptor for report 85;
it does not require a fabricated response to that factory operation.

The calibration report is 41 bytes with zero biases and interleaved gyro endpoints.
Gyro speed calibration is +/-512 degrees/s for +/-8192 counts, matching the
encoder's 16 counts per degree/s. The previous +/-8192 degrees/s magnitudes made
hosts interpret motion 16 times too large. Acceleration remains 8192 counts/g.
Pairing replies use a stable, distinct, locally administered unicast address per
slot in the host's expected byte order. Successful reads clear the caller's tail.

## Evidence and regression checks

Analyzed executable SHA-256:
`9F518C6D88D3A57F1EEAE4CE7708C666C0076331CBEFEDD3AD1FE4D219555195`.
Addresses below are RVAs, independent of ASLR:

- `25B43A0`: early DualSense USB marker check; enumeration branches on its result
  at `25B9130`.
- `25B4520`: reader transport classification using features 85 and 20.
- `25B676B`: firmware-dependent selection of modern versus legacy calibration
  feature usage. `25BAE69` reads the firmware word from report offset 28.
- `25B3010`: native USB sensor initialization; sets report 08, command 02 and
  calls HidD_SetFeature with length 48 at `25B317C`.
- `25B6926`: calibration read. `25B7460`: pairing read and wire-byte reversal.

`test_ds5_usb` validates parsed HID feature identities, lengths, buffer boundaries,
firmware/layout agreement, command validation, pairing identity, and host motion
calibration math. The profile regression test also calibrates an actual emitted
gyro report back to the original one-degree/s request.

`probe_ds5_usb` creates only slot 7 and selects its newly appearing VHF HID path,
then checks the native initialization sequence and receives a Cross input report.
It destroys its controller on completion; occupied slots fail without displacement.
The probe is manual, never an automatic CTest.

On 2026-09-13, the live 0.1.0.33 probe reproduced the missing USB marker, incorrect
pairing usage, missing/rejected sensor command, firmware/layout mismatch, gyro
scale, address, and stale-tail failures while ordinary button reads still worked.
All probe checks passed after installing locally signed driver 0.1.0.34. The DS4
live probe also passed. Both local source checkouts passed the MSVC test suites;
the driver built with WDK 26100 and passed INF, catalog, and signature verification.

A read-only snapshot of the running Resonance process showed no occupied native
scePad slots before the update. While our temporary neutral DualSense was held
open after the update, the game registered 054c:0ce6 and parsed firmware 0001003E
into its native controller record. This verifies native recognition; manual
gameplay and feedback behavior remain separate from that observation.

Protocol references:
- [Captured DualSense USB descriptor](https://github.com/nondebug/dualsense/blob/main/report-descriptor-usb.txt)
- [Linux hid-playstation calibration and firmware parser](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c)
