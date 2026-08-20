# Profile contract

`libvirtualgamepad` deliberately separates *a controller name selected by a
client* from *a controller profile implemented by the driver*. The protocol
enumerates the desired profile so Vibeshine can preserve its controller
selection rules, but the driver rejects every profile that has not completed
the acceptance criteria below.

This protects users from a common virtual-controller failure mode: presenting a
device with a familiar vendor/product ID while its report descriptor, feedback
behavior, and game compatibility do not actually match that device family. The
rule is about behavior matching the claim. It is not a prohibition on using a
family's identifiers when the identity is a necessary part of a complete,
tested emulation.

## Initial profile

The initial driver implements only `generic_hid`:

- a standard HID Game Pad collection;
- 32 buttons, hat switch, four signed 16-bit axes, and two unsigned triggers;
- a small generic vendor-defined output report for rumble and RGB feedback;
- one virtual HID child per owned controller slot;
- bounded, last-event-wins feedback polling to Vibeshine.

It is a VHF and protocol proof, not an assertion of native XInput support.

## Adding a profile

Before changing `find_profile()` to return a descriptor for a new profile,
include all of the following in the same reviewed change:

1. A descriptor and report mapping written from independent, permitted sources.
2. Unit-level serialization/parsing tests, including malformed output reports.
3. Controller lifecycle tests: create, submit, output feedback, disconnect,
   and reconnect.
4. Real Windows HID enumeration evidence and at least one target application
   compatibility result.
5. A documented capability table: buttons, sticks, triggers, touch, motion,
   battery, LEDs, rumble, trigger rumble, and any feature reports.
6. A privacy and provenance review for the descriptor and protocol data.

## Xbox profiles

VHF publishes HID children, and an HID child is not an XUSB or GIP endpoint.
That is not the end of the story, because XInput on Windows is reached through
a filter rather than a bus: `xinputhid.inf` attaches `xinputhid.sys` to HID
devices matching `HID\VID_045E&PID_xxxx&IG_00`, and `VHF_CONFIG.HardwareIDs`
lets a VHF child carry such an ID.

`xbox_series` is implemented on that route. It clears this contract because the
identity ships with the behavior: the native report shape, 10-bit triggers, the
1-based hat with a null state, the real button gaps, Share as a Consumer Record
usage, and the four-motor rumble payload with its actuator-enable mask. An
identity alone would not have cleared it, and a descriptor-only change still
does not.

`xbox_360` is refused outright. A real Xbox 360 pad is an XUSB device on a USB
bus, `xusb22.sys` binds to a bus child, and VHF cannot create one, so no
descriptor makes that profile honest.

`xbox_one` is implemented and reaches XInput the same way. It matches
`xinputhid.inf` on its own product ID (`PID_02EA`), where `xbox_series` has to
fall through to the generic GIP software ID because `PID_0B12` is not in the
INF's list at all.

Its report is the Series report without the Share button, which is the only
difference between the two pads that HID can see. Rather than keep two copies
of a descriptor in step by hand, the Xbox One descriptor is the Series one with
the Consumer Record block removed, and a test performs that removal and
compares. An edit to one that is not mirrored in the other fails the test
instead of shipping two pads that disagree about their own stick range. The
encoder does the same thing: it builds a Series report and keeps the prefix.

## What DirectInput does with the generic profiles

Measured on the generic PID profile, since these are claims worth checking
rather than assuming:

- DirectInput **does not** enumerate it under `DIEDFL_FORCEFEEDBACK`, and
  `DIDC_FORCEFEEDBACK` is clear. Publishing the PID report set is evidently not
  sufficient for DirectInput to accept a device as force-feedback capable.
- It appears **twice** in a plain controller enumeration, because the PID report
  set forms a second top-level collection.
- A resting trigger reads at the centre of DirectInput's range rather than the
  bottom, and travels only across the upper half. The HID declaration is a
  correct unsigned 0..255, so this is DirectInput's own axis handling.

The force-feedback result is the important one: force feedback under
DirectInput is the only reason this profile exists over `generic_hid`. Until
that is understood, `generic_pid` stays a fallback that the automatic ladder
reaches only when no console profile is offered, and it is deliberately not
offered as a user-selectable option. Advertising a force-feedback pad that
DirectInput refuses to treat as one would be worse than not offering it.

## PlayStation and Switch profiles

`dualshock_4`, `dualsense`, and `switch_pro` are implemented, each with an
independently derived descriptor and its own feature behavior: calibration,
pairing and firmware reads on the PlayStation pads, and the full USB handshake,
subcommand set and emulated SPI flash on the Switch pad.

Output reports are checked against the byte offsets the real controllers use,
not against our own structs. `offsetof` assertions in `dualsense.h` and
`dualshock4.h` pin every field, and the unit tests decode buffers filled at
literal offsets, so a transposed pair of fields fails the build or the test
rather than reaching a user. That distinction matters here because a wrong
offset is invisible at runtime: the light bar simply lights the wrong colour, or
a game's light rumble arrives on the heavy motor.

The left motor is the low-frequency one and the right is the high-frequency one,
on both pads. Only their report layouts differ.

## How a host writes an output report

Windows has two ways to send one, and this driver answers exactly one of them:

- `WriteFile` on the device handle **works**. This is what hidapi and most
  games use.
- `HidD_SetOutputReport` is **refused** with `ERROR_NOT_SUPPORTED`. VHF offers a
  write-report callback but none for the control-transfer path, so there is
  nothing for the driver to answer with. A real controller accepts both.

A host that only ever calls `HidD_SetOutputReport` therefore gets no rumble and
no light bar, silently. Nothing in this repository can change that; it would
take a VHF callback that does not exist. Worth knowing before chasing a
"rumble does not work" report that turns out to be the host's choice of API.

## Deliberately not implemented

Recorded so these are not repeatedly rediscovered as gaps.

**PID Custom Force Data and Download Force Sample.** These upload arbitrary
force waveforms rather than a parameterised effect. The engine here reduces
every effect to an amplitude on one of two rumble motors, so a custom waveform
would be flattened to a single amplitude anyway - losing the one thing that made
it custom. The effects that do map onto a motor (constant, ramp, periodic, and
the conditions, which are accepted and silent) are implemented. Titles using the
sample-upload reports are rare, and the profile they would apply to is a
fallback DirectInput does not accept as force-feedback capable in the first
place.

**Xbox 360.** Not possible, as above: it needs a bus child that VHF cannot
create.

**Dual touchpad (`LI_CCAP_DUAL_TOUCHPAD`).** Blocked upstream rather than here.
Sunshine has no touchpad-index plumbing at any layer, so there is nothing for a
second touchpad to be wired to. The DualSense and DualShock 4 profiles already
report two contacts on their single touchpad, which is what real hardware does.
