# VHF keyboard and mouse producer

The Windows driver creates a VHF keyboard and mouse device through the private
control interface. The independently versioned `lvg::km` ABI does not change
the gamepad ABI. The client must query keyboard/mouse capabilities before use;
older driver packages can reject that query.

The UMDF project enables keyboard/mouse by default. It supplies zero vendor and
product IDs, which VHF accepts as optional values; project-owned IDs can be
provided through the project properties. The control operations are restricted
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

Relative and absolute pointer reports use separate Mouse top-level collections
because Windows mouhid rejects duplicate X/Y capabilities in a single collection.
Only the relative collection owns buttons, including during absolute drags; the
absolute collection contains movement and constant padding. HID coordinates use
0..32767 because Windows' scaled-value parser rejects the upper half of an
unsigned 16-bit range; the control API remains normalized to 0..65535. The host
must confirm desktop button/position delivery when crossing mouse collections,
since submission completion acknowledges queue admission rather than OS delivery.
The absolute pointer
uses normalized 0..65535 coordinates in the **primary
display** domain. The reported `primary_absolute_mouse` capability identifies
that domain; it does not promise virtual-desktop coordinates. The host must map
from its capture geometry and handle off-primary positions separately. This
mapping and Windows HID event ordering need runtime qualification on supported
Windows configurations, including multi-monitor, DPI, desktop transitions and
pointer drags. Consumer HID has no portable arbitrary-Unicode representation.

See [the transport contract](keyboard-mouse-source-transport.md) for request
validation, ownership, power transitions and cleanup. The producer release
must be built, packaged and signed through the established driver pipeline;
changing source alone does not update an already installed driver.
