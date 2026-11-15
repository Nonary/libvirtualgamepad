# VHF keyboard and mouse producer

The Windows driver creates a VHF keyboard and mouse device through the private
control interface. The independently versioned `lvg::km` ABI does not change
the gamepad ABI. The client must query keyboard/mouse capabilities before use;
older driver packages can reject that query.

The UMDF project enables keyboard/mouse by default. Production packages require
explicit nonzero project-owned vendor/product IDs supplied through the project
properties. The mapper binds the exact `HID\\VID_xxxx&PID_yyyy&REV_0100&Col02`
identity and verifies vendor usage `0xff53/1` and the producer container. Zero
IDs remain source/fixture defaults and do not establish a usable mapper binding. The control operations are restricted
to enabled administrators and LocalSystem. Each keyboard/mouse request must
come from the process that opened its device handle and pass the token check.
Duplicating or inheriting that handle into another process does not transfer
input privileges. A retained process handle with limited-query access prevents
reuse of an exited opener's PID until the device file closes. It is acquired
under client impersonation, explicitly enabled by both the client and INF.
Gamepad permissions remain
unchanged.

The endpoint implements a full-state keyboard bitmap, five mouse buttons,
relative mouse counts, vertical and horizontal wheel detents, and consumer-page
mute, volume, play/pause, next, previous and stop controls. One ordered bounded
queue carries keyboard, button, relative, absolute and consumer reports.
Backpressure does not change held state. Disconnect cleanup emits neutral
keyboard, button and consumer reports before deleting the child when possible.

All mouse input uses one vendor-defined HID transport collection, usage page
`0xff53`, usage `1`. The owned kernel mapper converts its ordered reports into
one Mouclass stream. Relative movement, full button snapshots, wheel units, and
normalized absolute coordinates therefore share one ordering boundary. Absolute
reports use `MOUSE_MOVE_ABSOLUTE | MOUSE_VIRTUAL_DESKTOP` and the complete
`0..65535` domain; no primary-monitor remapping or desktop cursor observation
acknowledgment is involved.

The producer advertises `ordered_mouse` and `virtual_desktop_absolute` with
coordinate-domain generation `2`. It does not advertise the older relative,
primary, or bound-absolute mouse capabilities, so an old host cannot mistake
this transport for the old Mouhid collections. Keyboard and consumer control
ABI version 1 and their report layouts remain unchanged. The host must require
the new mouse capabilities. Opening the child must confirm that its exact
container has a mapper connected to Mouclass, in D0, with its HID reader ready;
a missing mapper causes synchronous child deletion before open fails.

`include/libvirtualgamepad/mouse_transport.h` defines the packed little-endian
22-byte report. Button-only reports contain zero relative displacement rather
than a remembered absolute target. Sequence values identify admission order,
wrap from `0xffffffff` to `1`. Normal reports must be contiguous. Normal
close emits a strict-neutral purge marker (flag `4`, all buttons, coordinates
and wheel values zero) to account for discarded queued reports; only this
marker permits a forward sequence gap. Unmarked gaps fault instead of silently
losing input. They are not acknowledgments of desktop delivery. The mapper returns an
8-byte output report (ID 5, version 1, zero reserved field, exact sequence)
only after every resulting mouse packet entered the Mouclass queue. The
producer allows one unacknowledged mouse report, independent of VHF readiness
and keyboard/consumer report retirement. This prevents the mapper's serialized
HID reads from overflowing a per-file HIDClass ring. VHF readiness only retires
producer buffers; it does not establish where the cursor moved. Normal close
retains any in-flight mouse acknowledgment before sending its neutral mouse
report; forced abort still removes the child if that acknowledgment stalls.
Native Windows qualification must cover multi-monitor/DPI/desktop transitions,
clipping/recentering, mixed-mode drags, partial class consumption and teardown.
Consumer HID has no portable arbitrary-Unicode representation.

See [the transport contract](keyboard-mouse-source-transport.md) for request
validation, ownership, power transitions and cleanup. The producer release
must be built, packaged and signed through the established driver pipeline;
changing source alone does not update an already installed driver.
