# Keyboard/mouse HID producer prerequisites

This source-only implementation prepares the feature requested by
[Nonary/Vibepollo#602](https://github.com/Nonary/Vibepollo/issues/602). It does
**not** add a usable Windows input backend or change the released driver's
capabilities. The recovered packet was prepared against libvirtualgamepad
v0.1.0-beta.9, `7b000a2b060b4daeb0b5c6aa7b77ae1d81857e7a`, protocol 2;
this branch does not change or attest a current consumer pin. The new portable
`lvg::km` ABI version 1 is a separate proposed extension, not a replacement for
that existing controller protocol. The optional source stage adds IOCTL/client/WDF/VhfUm connection and a
conditional project entry; it defaults OFF, assigns no accepted VID/PID, changes
no INF/release/pin/signing or install action, and advertises no shipping feature.
See [the source transport contract](keyboard-mouse-source-transport.md).

`driver/src/keyboard_mouse.cpp` provides real descriptor and encoding/FIFO
behavior. `driver/tests/test_keyboard_mouse.cpp` parses that descriptor and
executes the actual core. This is useful preparatory source work; portable tests
cannot qualify kbdhid/mouhid, VHF callback scheduling or a Windows build.

## Producer behavior

One endpoint belongs to one already-authorized private-control connection. An
internal nonzero owner token is associated with the WDF file object; it is not a
client-supplied wire field. The transport must first validate the exact input
buffer length, then the fixed ABI/header/reserved fields, then call the core.
The `submit` overloads validate the typed values/header; they cannot validate an
unmarshalled raw buffer. Unknown owners, unsupported absolute binding, malformed
requests and stale target generations accept no event.

The keyboard is a page-0x07 bitmap for usages 0..0xE7. Usages 0..3 and >0xE7 are
refused. Each report is a complete state snapshot and supports more than six
held keys, including independent left/right modifiers. An identical down is
unchanged; the adapter must disable its existing manual key-repeat timer for
HID-bound keys and let the native keyboard stack own typematic. Consumer/media
keys and Unicode text need the existing backend; they must not be guessed as
page-0x07 usages. This descriptor makes no LED/output-report promise.

Relative mouse counts use signed 16-bit X/Y, positive right/down, five buttons
in HID order, signed whole wheel and AC Pan detents in -127..127. Host Win32
wheel units must be accumulated and divided by 120; subdetent remainder is kept
per input owner and cleared on disconnect. Larger counts/detents are split into
ordered accepted requests, committing only the accepted part on backpressure.
HID counts still pass through Windows pointer policy; no pixel precision or
pointer-acceleration equivalence to SendInput is claimed.

The absolute collection carries unsigned 16-bit X/Y only, so it cannot release
a held relative-mouse button. It has no inherent knowledge of the active display,
DPI, virtual-desktop origin, rotation or `MOUSE_VIRTUAL_DESKTOP`. A verified
coordinate-domain binding must exist before a nonzero target generation is
passed to `open`; without it, absolute submit returns unsupported. Every absolute
request must carry that exact generation. A topology/DPI/rotation/target change
requires quiescing and recreating/rebinding the endpoint. A normalized transform
alone does not qualify this binding.

All reports use one ordered bounded FIFO. Relative deltas and quick key/button
press/release pairs are never replaced by a newer snapshot. At capacity 64,
submit returns busy before changing any held state; the caller must retry or
terminate the endpoint, never silently drop the accepted-state accounting.
A real readiness callback grants one report credit. Returned storage stays
unchanged until the next readiness callback or synchronous device deletion.
The existing controller report pump is deliberately not reused: it coalesces
continuous reports and drops old transitions when its queue is full.

Only the owner may close. Close rejects new input, discards unconsumed queued
motion/presses, and appends neutral keyboard and button reports. It preserves an
already retained report. State becomes drained only after the two neutral reports
have been retired by subsequent readiness callbacks. The caller then synchronously
deletes the VHF child and calls `reset_after_device_deleted`. A failed or uncertain
submission instead faults the endpoint and pins retained storage until deletion;
it never invents a read credit or replays the event through SendInput. A forced
PnP stop/process death cannot wait forever for neutral reports: delete the child
synchronously and validate Windows removal cleanup natively before release.

## Missing connection to Windows

The default-OFF native source stage now connects the core to actual UMDF2/VhfUm
file ownership, versioned METHOD_BUFFERED dispatch, a dedicated VHF child,
readiness and forced cleanup/removal, plus client transport on the existing
control handle. Its query exposes only keyboard/relative/wheel with absolute
generation 0, independently from protocol 2. The default/released driver refuses
the new controls. This is source progress, not native SDK/framework/runtime or
consumer activation proof. Exact contracts and remaining gates are in
[keyboard-mouse-source-transport.md](keyboard-mouse-source-transport.md).

A project-owned device identity is required. This preparation does not borrow
Microsoft/Sony/controller identities or invent an accepted VID/PID. Descriptor
parsing tests do not establish Windows mapper attachment, Raw Input identity,
mouse cursor visibility, secure-desktop behavior or anti-cheat acceptance.

## Release/consumer gates

1. Finish and independently review WDF/IOCTL/client integration and Windows
   SDK/WDK compilation. Exercise production create/start/ready/close/delete and
   fault callbacks with deterministic transport/WDF boundaries.
2. Qualify a project-owned identity and native absolute target binding. Test
   primary/nonprimary monitors, negative origins, mixed DPI, rotation, topology
   changes, desktop switching, relative/absolute alternation and button drags.
3. On current Windows test keyboard layouts/AltGr, left/right modifiers, keypad
   Enter/navigation, Pause/PrintScreen, >6 keys, typematic, Unicode/media fallback,
   quick taps, queue saturation, stream disconnect, process crash and PnP removal.
   Validate per-client shared-key ownership through the host's existing refcounts.
4. Publish a producer artifact only after those gates. Preserve immutable
   source/tag/archive hash/DriverVer/protocol metadata. Existing consumer policy
   ingests an unsigned archive, verifies its catalog membership, and signs the
   catalog plus setup tool through the consumer MSI signing request. Do not
   Authenticode-sign the catalog-bound DLL after generating its catalog.
5. Consumer integration may then update pins and wire the opt-in UI/runtime adapter.
   Default is SendInput. Missing/mismatched producer capabilities, unqualified
   release, failed preflight or unverified absolute domain select the existing
   backend before any HID input. A fatal error after HID acceptance requires
   owner teardown and held-input cleanup before a new backend is selected.

No driver was built, loaded, installed, signed or published by these tests.
