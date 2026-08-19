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

`xbox_one` is reachable by the same route as `xbox_series` and stays
unavailable only until its own report shape and feature behavior are
implemented and tested.

## PlayStation and Switch profiles

`dualshock_4`, `dualsense`, and `switch_pro` remain unavailable until each has
an independently derived descriptor and complete feedback/feature behavior.
Those profiles may be HID-native, but still need their own output, touch,
motion, battery, and compatibility validation. No source or proprietary assets
from LizardByte's Windows driver are used as an implementation input.
