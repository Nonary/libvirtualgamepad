# VHF keyboard and mouse producer

The Windows driver creates a VHF keyboard and mouse device through the private
control interface. The independently versioned `lvg::km` ABI does not change
the gamepad ABI. The client must query keyboard/mouse capabilities before use;
older driver packages can reject that query.

The UMDF project enables keyboard/mouse by default. It supplies zero vendor and
product IDs, which VHF accepts as optional values; project-owned IDs can be
provided through the project properties. The control operations are restricted
to enabled administrators and LocalSystem, and each request is checked under
the requesting security context. Gamepad permissions remain unchanged.

The endpoint implements a full-state keyboard bitmap, five mouse buttons,
relative mouse counts, vertical and horizontal wheel detents, and consumer-page
mute, volume, play/pause, next, previous and stop controls. One ordered bounded
queue carries keyboard, button, relative, absolute and consumer reports.
Backpressure does not change held state. Disconnect cleanup emits neutral
keyboard, button and consumer reports before deleting the child when possible.

Relative and absolute pointer reports share one Mouse top-level collection and
each carries the current button state, so a drag is not split between distinct
mouse devices. The absolute pointer uses normalized 0..65535 coordinates in the **primary
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
