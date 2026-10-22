# Optional keyboard/mouse UMDF2 source prerequisite

This is an **incomplete, default-OFF source prerequisite** for
[Nonary/Vibepollo#602](https://github.com/Nonary/Vibepollo/issues/602), stacked on
[libvirtualgamepad PR8](https://github.com/Nonary/libvirtualgamepad/pull/8).
It is not a complete issue fix, usable host backend, Windows-qualified driver,
accepted device identity, signed artifact, deployed feature, or nightly inclusion.

## Provenance and dependency

The recovered producer packet is the diff from
`d8161a3f27936e3c603db9072735e694aab263f6` to original producer commit
`8ee4b47d59760b0badff0dbd84f9ccf57b491ea9`. Its verified patch SHA256 is
`10f1acd382fb7d17af57545a6c0eedf315bfdeb1efbba292d36df6cadd4f67f4`.
This adaptation started from PR8 head
`fd67a37554f17fce3c6ad586f6acb20f7a48cb8d` and was rebased onto its narrow
probe repair, `3dc2bb48667ae898a337691828f07a6e0ab216ba`, not onto the
historical parent. The published producer history then incorporated PR8 head
`630631b8c118e3d989a3d305435aba6146d2508b` (probe scope-exit cleanup plus
main `30bc8d8b840ec0cc4479e446ddb6e4c7e9fe8c60` / unchanged merged PR7).
Only additive CMake test entries needed conflict reconciliation; KM behavior
was unchanged by that dependency update.
The packet could not safely be applied unchanged: it admitted KM work while
PR8's device was outside D0 and omitted the KM child from D0 retirement.

This branch changes producer/client source only. It does not import the host
packet or change a consumer gitlink. Host integration must wait for/reconcile
[PR333's shared-handle recovery](https://github.com/Nonary/Vibeshine/pull/333):
closing/reconnecting `lvg::client` also destroys KM ownership. A later host change
must quiesce/invalidate the router before handle replacement, preserve canceled
held-input accounting, and re-query/re-qualify the new connection. It must never
replay accepted or uncertain HID input, or interpret gamepad recovery as KM
neutral delivery. Source migration to Vibepollo is a separate step. PR7's touch
fix is independent, except for additive test CMake reconciliation.

## Off by default and still fail-closed

`LVG_ENABLE_KEYBOARD_MOUSE_SOURCE` defaults to 0. The project properties
`LvgEnableKeyboardMouseSource` and
`LvgKeyboardMouseNativeContractAcknowledged` both default to false. The driver
DLL excludes the new core unless explicitly enabled. Existing gamepad protocol 2
and its feature negotiation are not replaced. Additive client methods are never
called by `connect()` or implicitly by controller operations.

An experimental source build requires both properties set to true and separately
reviewed, nonzero 16-bit `LvgKeyboardMouseVendorId`/`LvgKeyboardMouseProductId`
values. No default identity is supplied. Numeric checks and an acknowledgement
are build guards, not evidence of ownership or native qualification. Tests use
synthetic sentinel IDs only in in-process shims. Do not publish those identities.
No INF, packaging, signing, release, consumer pin, UI, or SendInput route changes.

Query advertises only keyboard, relative mouse, and wheel detents, with absolute
target generation 0. Absolute submissions remain unsupported: normalized
coordinates are not a verified native coordinate-domain binding. The recovered
host selector requires all capabilities, a matching nonzero absolute generation,
and explicit release/native qualification. Those gates must not be weakened to
make this source stage appear usable.

## Separate control ABI

`keyboard_mouse_control.h` allocates METHOD_BUFFERED IOCTL functions
`0x820` through `0x827` with read/write access: query, open, close, keyboard,
button, relative, absolute, abort. KM ABI version 1 is separate from controller
protocol 2. The default/released path refuses the new controls.

Both callback buffer lengths and retrieved raw-buffer lengths must match exactly.
Input is copied into a fixed local value before output writes, including aliased
METHOD_BUFFERED buffers. Header size/version/reserved fields and typed values
are checked before mutation. Failures complete with Information 0.

Submission requires a validated 16-byte reply: prefix, accepted (0) or busy (1),
and zero reserved field. Reply storage is checked before admission. Busy means
no event or held-state change was accepted; unchanged duplicates count as
accepted. Only that validated reply becomes client `ERROR_BUSY`. A raw transport
busy becomes `ERROR_NOT_READY`, since delivery is uncertain. Malformed/truncated
replies and native submit failures are also uncertain. They never authorize
SendInput replay. Close busy means neutral drain is pending, not submission
backpressure.

## Ownership, D0, and recovery

One endpoint is owned by an actual WDF file object, not a wire-supplied token.
The dedicated KM FIFO, retained report, transfer packet, and callback activation
are separate from controller report storage. WDF's raw context is explicitly
placement-constructed under source opt-in. File cleanup marks its owner before
waiting on the lifetime gate, so an in-flight creator cannot reopen it.

D0 exit publishes `powered = false` before waiting for create/start serialization.
KM query/open/submission, readiness credit, and pump admission all require D0.
Controller callback admission also observes this early power closure. After a
successful VhfCreate, KM checks power/cleanup again before VhfStart and after
Start returns. A transition racing either call cancels adoption and synchronously
deletes the created child before resetting its storage. Release/target cleanup
similarly publishes stopping intent before waiting on a creator.

D0 retirement invalidates the KM generation and closes callback/report admission.
It drains already reserved submit owners and the pump, then deletes KM before
controller deletion. The shared local target and file handle remain valid through
that rundown. Retained bytes are never reset before native deletion. No path
waits for neutral HID consumption during forced power/removal cleanup.

After D0 deletion the core, queued input, retained storage, held keys/buttons,
and callback pointer are cleared. A `lost` slot retains only diagnostic owner
identity. That owner's submit/close returns `STATUS_DEVICE_REMOVED`, not a
successful neutral-delivery acknowledgement. Other owners cannot take over a
lost lease. D0 entry does not recreate children or replay input. The same owner
must explicitly open again, or abort/file cleanup can release the lost slot.
A file marked for cleanup cannot reopen; abort is idempotent after removal.

Reopen checks the old PnP instance's presence outside driver locks, then rechecks
power, owner state, and the captured slot generation under the lifetime gate.
A failed/malformed enumeration or lingering instance returns busy. Even an entire
off/on cycle while enumeration is in flight invalidates the reservation. The
caller may retry; the sequential control queue never polls waiting for removal.
The KM generation is private activation state and does not change ABI version 1.

## Callback and retained-report contract

The short-held rundown SRW lock is ordered before `state_lock`. One pump reserves
handle, packet, retained report and a submit owner, releases all local locks, then
calls VhfReadReportSubmit. Request paths release the lifetime gate before pumping.
A single pump handles reentrant/concurrent readiness, and atomically disarms when
it observes no report so readiness cannot be stranded. Failure faults the FIFO
without manufacturing credit or resetting retained bytes.

Stop retires the per-activation context, blocks new admission, and waits with the
rundown lock released until the pump and submit owners finish. A failed condition
wait falls back to polling with that lock released. No state/rundown lock spans
VhfStart, VhfReadReportSubmit or VhfDelete. Callbacks never take the lifetime gate.
Admitted callback entries are counted and drained before their context is freed.

The VHF documentation requires producers to stop initiating requests before
[VhfDelete](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/nf-vhf-vhfdelete),
which can delete an unstarted handle. The readiness buffering contract permits
one submission per callback and retains the transfer buffer until the next
[ready notification](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/nf-vhf-vhfreadreportsubmit).

**Unqualified native assumption:** reclaiming the per-activation context relies
on VhfUm quiescing ready-callback entry by synchronous delete return, including
already-dispatched entries not yet counted by this source. The general reference
pages describe VhfKm and do not establish that exact VhfUm guarantee. The fixture
models it, not proves it. The native acknowledgement remains false by default.
An OS call/submit that never returns can hold synchronous rundown open; this
source does not claim a driver-enforced timeout or a native watchdog fix.

Normal close purges queued input, appends neutral keyboard/buttons, and preserves
the in-flight report until readiness. Only after the second neutral is retired
does a subsequent close delete and succeed. Abort force-removes KM without
closing the shared client or controller children. Abort success proves removal,
not neutral delivery; a failed abort leaves client-side removal unproven.

## Native gates and validation

The project is an UMDF2.15 DynamicLibrary using WindowsUserModeDriver10.0,
SDK/WDK 26100 and VhfUm.lib. It remains an upper filter above inbox Vhf.sys.
PR8's non-power-managed default queue is preserved. Under KM opt-in the file
configuration sets AutoForwardCleanupClose false because private creates are
completed here. Null internal-target creates and private owner controls still
need real UMDF validation; portable tests are not OS behavior evidence.

Tests execute actual driver.cpp/client.cpp bodies, core FIFO/descriptor encoders,
and deterministic request, Configuration Manager, WDF and VHF boundaries. They
cover default-OFF refusal, opt-in initialization, exact/aliased/malformed buffers,
backpressure, neutral retirement, target/create/start/submit failure, create/start
versus D0, mixed controller/KM retirement, reserved submissions versus deletion,
cleanup during power loss, stale callbacks, delayed PnP removal, and explicit
re-entry without replay. Stale-context tests use separate live fixture storage,
not use-after-free invocations forbidden by the modeled native contract.

See [the validation record](keyboard-mouse-validation.md) for executed checks and
negative controls. Remaining prerequisites are independent review, real SDK/WDK
builds, native callback/UMDF contract qualification, accepted project identity,
absolute-domain binding, verifier/power/disconnect/process-death tests, Windows
held-input cleanup, typematic/layout/Raw Input/multi-client coverage, signed
producer release and qualified consumer integration. No Windows runtime, WDK,
VM or hardware qualification was available for this source adaptation.
