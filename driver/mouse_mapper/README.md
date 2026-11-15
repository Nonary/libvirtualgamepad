# Ordered mouse mapper

`VibeshineVhfMouseMapper.vcxproj` is a KMDF mouse port/mapper, built for x64
or ARM64 using WDK 10.0.26100. It is the function driver for the producer's
second HID collection. The INF matches only an explicitly configured, owned,
nonzero VID/PID, revision 0100, collection 02. Build and packaging must receive
the same `LvgKeyboardMouseVendorId` and `LvgKeyboardMouseProductId` values.
The source does not assign or claim ownership of a USB vendor identifier.

The mapper verifies HID collection attributes, usage FF53/0001, report length,
and the dedicated VHF container before starting. MouseClass's kernel CREATE
is forwarded to lower HIDClass to initialize a valid collection file object.
A WDF existing-device I/O target pairs that file object with the lower device
object. Opening the HID interface by name would reach the upper MouseClass
stack and cannot substitute for this lower read target.

The portable `mapper_core.h` converts the shared 22-byte vendor report into
up to three ordered class packets: button edges/movement, vertical wheel,
horizontal wheel. Wheels already use Windows wheel units. Its fixed storage
retains every unconsumed packet; the native adapter reads no subsequent report
until those packets are consumed. Normal report sequences must be contiguous
(including wrap past zero). A
forward sequence gap is permitted only for an explicitly marked, validated
neutral close report because producer close purges queued movement before
sending that release marker. The marker is relative, releases held buttons,
and is ACKed even if no button release packet is needed.
Duplicates, reverse/ambiguous sequence order, malformed reports, failed reads,
and impossible class consumption counts stop the stream.

A DPC calls the documented MouseClass service callback at
DISPATCH_LEVEL. A private spin lock serializes conversion, consumption, and
class callback access. A single pump owner and rerun latch serialize request
formatting and sending even if the same DPC is dispatched on another processor.
An owner processes at most 16 iterations before queuing a continuation.
A passive lifecycle lock serializes file and power operations. The 1 ms timer
is used only when MouseClass consumes a partial or empty prefix; normal
read/ACK completion queues the DPC immediately.

Every successful DPC enqueue and retry-timer arm reserves an explicit rundown
reference before scheduling; the callback releases it as its last driver
access. Cancelling queued work releases only that queued reservation. Each
I/O send similarly reserves a separate rundown reference before formatting
and releases it last in its completion callback (or failed-send path).
Suspend first marks the stream inactive and cancels queued timer/DPC work,
then waits for the pump rundown domain to close every reservation/send window.
It cancels the lower target and waits for the I/O rundown domain, including
callbacks for requests which had already left the target sent list. Resume
reinitializes both domains only after this drain. A completed report and
partially consumed packets survive suspend. The lower
HID file is released only after reads and ACK writes have been drained.
The documented mouse DISCONNECT request returns STATUS_NOT_IMPLEMENTED;
PnP removal performs rundown and clears the saved class connection.

A direct raw child PDO (`VIBESHINE\ORDERED_MOUSE_STATUS`, instance 0) exposes
only the readiness IOCTL defined in `mouse_mapper_control.h`. Its container is
the same as its HID parent. SYSTEM, Administrators, and LocalService have read
access. This bypasses MouseClass's exclusive user-open path using the same
sideband pattern as Microsoft's KMDF keyboard-filter sample. It supplies no
input-injection or raw-HID-read interface.

Run portable tests with:

```text
cmake -S driver/mouse_mapper -B build/mouse-mapper-tests
cmake --build build/mouse-mapper-tests
ctest --test-dir build/mouse-mapper-tests --output-on-failure
```

These tests cover conversion, all 1024 button-state transitions, partial/zero
consumption, bounded admission, signed wheels, mixed coordinate modes,
sequence wrap/marked purge gaps, rejection of unmarked gaps, and malformed
reports. They do not qualify the KMDF adapter. A WDK build, INF validation,
signed package,
native installation, Driver Verifier/power/removal testing, and desktop/game
ordering tests remain necessary before native qualification.

After MouseClass consumes every packet, the mapper writes the shared 8-byte
report-5 ACK to the same lower HID collection. It starts no subsequent read
until that write completes successfully. The producer permits only one mouse
report to await this ACK, preventing its mouse stream from overrunning
HIDClass's input ring while MouseClass stalls. ACK failure or cancellation is
terminal and never replayed because the producer may already have received it.
A new VHF endpoint activation is required after an uncertain ACK.

The native callback test compiles the actual `mapper.cpp` against deterministic
WDF boundaries and exercises the lower-file binding, DPC fast path, partial
class consumption, ACK gating, inline read/write completions, status interface,
and power/file rundown. Tests also delay completions until after target Stop,
dispatch a second DPC during a reserved formatting window, pause a sending DPC
on another thread during suspend, and verify late timer/DPC callback rundown.
The Mouse-class function stack uses the framework's exclusive-open gate and
forwards every CREATE actually delivered to its callback, so automatic lower
CLEANUP/CLOSE forwarding remains balanced even for lower CREATE failures.
The raw status child is independently accessible.

The boundary model cannot replace a native WDK build
or establish installed Windows driver behavior.
