# DS4 USB compatibility

The Windows VHF DS4 profile implements our USB HID device contract in
`include/libvirtualgamepad/ds4_usb.h`. This portable header has no ViGEm or
Windows dependency. The Linux UHID implementation in Vibeshine is being
investigated separately; it has not been switched to this header in this change.

Raw Input recognition is insufficient validation. A Plague Tale: Requiem's
native controller library also validates feature capabilities and performs a
USB initialization sequence before consuming any input reports:

| Property/operation | USB contract |
| --- | --- |
| Input / output / maximum feature length | 64 / 32 / 64 bytes, including report ID |
| Firmware feature | ID A3, page FF80, usage 43, 48 data bytes |
| Pairing feature | ID 12, page FF02, usage 21, 15 data bytes |
| Calibration feature | ID 02, 36 data bytes: 37 total |
| Firmware protocol field at offset 35 | little-endian 4300 (native minimum 3100) |
| Sensor initialization | SetFeature ID 14, command 02, 17 bytes total |

Keep the USB feature identities intact. A small descriptor with generic vendor
usages can pass browser testing but fail native transport detection. Adding a
Bluetooth feature report to increase capacity would select the wrong transport.
The full USB descriptor already contains the larger vendor feature reports that
give HID its 64-byte maximum. Unimplemented factory/pairing/authentication
operations remain unsupported; they do not return fabricated successful data.

Calibration is synthetic and consistent with our emitted motion samples:
zero bias, 16 gyro counts per degree/second, and 8192 accelerometer counts per g.
The USB calibration endpoints interleave positive/negative values by axis.
Build date/time fields are empty because this is not a factory controller.
Pairing addresses are locally administered, stable, and distinct per slot;
native libraries deduplicate devices by this address. The pairing report also
carries the hardware `08 25 00` bytes after the client MAC. Dying Light's
libScePad 1.0.4.1 only reverses the six MAC octets, but later Sony PC libraries
overlay `ReportFeatureInMacAll` on the same 16-byte USB reply.

Successful feature reads clear the caller's remaining capacity. VHF can reuse a
report buffer, so copying only the short feature payload leaves stale bytes in
larger HID requests. Invalid or short requests leave the buffer untouched.

## Validation

```
cmake -S driver/tests -B build/contract-tests
cmake --build build/contract-tests --config Release
ctest --test-dir build/contract-tests -C Release --output-on-failure
```

`test_ds4_usb` checks native feature classification and initialization, exact
and oversized requests, failure boundaries, and host calibration calculations.
`test_pid_descriptor` also validates the actual driver profile wrappers and all
other controller descriptors/encoders.

On Windows, run `probe_ds4_usb` separately against the installed driver. It
creates only slot 7, reads Windows' parsed capabilities and feature responses,
enables sensor reports, submits a Cross press, reads the resulting HID packet,
and destroys the test controller. A occupied slot is an error; the probe does
not displace another controller. This is intentionally not an automatic CTest.

Local validation on 2026-09-13: MSVC and GCC contract/descriptor tests passed;
driver 0.1.0.33 compiled with the Windows 26100 WDK, passed InfVerif/Inf2Cat and
local-signature verification, and passed every live probe check after its root
device was re-created. The game was no longer running after the stream restart,
so a gameplay retest remains necessary. These checks do not establish that all
native games or Proton versions support every exposed feature.

Protocol cross-check: Linux's upstream
[hid-playstation driver](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c).
Native gates were recovered from Requiem executable SHA-256
`4A0D3D39EFB5BA880183B595EEF1EDB496E0CDAAA8ACFCE67A0CE01065375D6D`:
RVA 160823D (minimum 53-byte feature capacity), 1606820 (USB feature identity),
16083AD (firmware protocol revision), and 1605870 (sensor initialization).

Dying Light The Beast ships `libScePad.dll` 1.0.4.1 (SHA-256
`73d21bda527e53860b58c57de4a511507efaad4ddf3b4957ece4e49bc0791f84`).
That build accepts VID 054C with PID 05C4 or 09CC, requires
`FeatureReportByteLength >= 16` and USB input/feature sizes 64/53, reads
pairing report 0x12 at 16 bytes and reverses the MAC, reads firmware 0xA3 at
49 bytes and requires the LE16 at offset 0x23 `>= 0x3100`, then enables
sensors with SetFeature 0x14 command 02. It rejects a Bluetooth-style feature
0x06 (page FF04, usage 0x27, count 0x34).
