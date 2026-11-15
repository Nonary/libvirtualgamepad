# Ordered mouse driver packaging

The producer release remains the same **single-stage unsigned archive** contract
used by beta.10. A clean tagged build runs `tools/build-release-package.ps1`,
verifies the unsigned package, creates the ZIP/checksum/provenance records, and
publishes through the existing release workflow. There are no Microsoft signing
submissions, signing inputs, prepare/resume jobs, or production signature claims
in this producer release.

## Build and package identity

`tools/build-driver.ps1` builds the UMDF VHF source DLL, KMDF mapper SYS and
root-device setup EXE with Windows WDK tools. It stages the mapper separately as
`mouse_mapper/VibeshineVhfMouseMapper.inf`, `.sys`, and `.cat`, runs InfVerif on
both INFs, and generates both catalogs through Inf2Cat. The released package is
x64; the build script also accepts explicit ARM64 builds.

Keyboard/mouse VID and PID default to the existing virtual VHF identity **0/0**.
Optional `-KeyboardMouseVendorId` and `-KeyboardMouseProductId` values are
16-bit integers (0..65535) and are passed consistently to both driver projects
and mapper INF rendering. No new product allocation is required for these
virtual-device defaults. Both projects share the numeric validation in
`driver/MouseIdentity.props`.

The INF still targets exactly `HID\VID_xxxx&PID_yyyy&REV_0100&Col02`, including
`0000/0000` for the defaults. Source collection ordering and revision must match.
Runtime additionally checks the actual source container, collection capabilities
and mapper readiness handshake. A generic mouse/class-wide filter is not used.

## Unsigned payload and provenance contract

New archives declare `features.mouse_mapper=1`, `transport_version=1`, and
`mouse_mapper.kernel_signing=unsigned-producer`. The manifest records the actual
VID/PID and hashes every INF/DLL/SYS/CAT/setup artifact. Fresh Inf2Cat evidence
binds all six catalog-related files: the source INF/DLL/CAT and mapper
INF/SYS/CAT. Release metadata, archive SHA-256 and source revision retain the
same provenance responsibilities as the prior producer release.

Both mapper CAT and SYS are in `signing.unsigned_payloads` and must have
Authenticode status `NotSigned` with no signer when released and ingested.
They are **not** in `signing.signed_downstream`. The existing consumer MSI
signing request continues to own only the UMDF CAT and root-device setup EXE.
The catalog-bound source DLL and all mapper bytes remain hash-pinned; changing
or signing a mapper artifact requires a separately reviewed package/provenance
update, not an exception to these hashes.

The downloader and package refresh verify upstream hashes, exact layout, fresh
catalog evidence and `NotSigned` status. They do not request `/kp` or require a
Microsoft signature simply to publish or ingest this unsigned producer archive.
A daily package can locally sign the existing UMDF CAT/setup EXE while copying
the unsigned mapper unchanged, preserving its explicit unsigned metadata.

## Installation remains a separate trust boundary

Unsigned publication does **not** establish Windows kernel trust. The final
production MSI qualification and installer retain their kernel-policy/catalog
membership checks. The installer refuses an `unsigned-producer` mapper rather
than treating the signed MSI container or ordinary UMDF signing as KMCS. A
future native production deployment may need a separately trusted kernel-signed
mapper package; that signing work is outside this unsigned producer release.
No script changes Secure Boot, enables test signing, or imports arbitrary
certificate trust to bypass that boundary.

Local test builds can explicitly sign their mapper SYS before regenerating and
signing its catalog. Existing exact-certificate test-host consent and Windows
security requirements still apply. Optional signed-package verification is a
separate explicit mode, not a prerequisite of the unsigned release workflow.

For a compatible trusted package, an equal active parent does not skip mapper
validation: the mapper is staged independently, and equal stored mapper versions
are retried for missing bindings. An equal parent present only in Driver Store
still reaches setup to create and verify its missing root after mapper staging.
Any strictly newer active/stored parent fails before mutation because this
candidate cannot attest a future parent's identity/transport; a matching package
at least as new is required. Newer mapper versions are never downgraded. Final
cleanup removes the owned root; Driver Store package removal remains explicit.

Bundled host candidates require the explicit mapper feature by default and in
CI. Legacy verification utilities still accept immutable archives without that
feature. Actual new release tags/source/DriverVer/protocol/archive pins must be
updated from the real published artifact; this document supplies no invented
release identity or binary hashes.
