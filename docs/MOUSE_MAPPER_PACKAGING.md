# Ordered mouse driver packaging and signing

The ordered mouse transport requires two independently installed driver packages:
`driver/VibeshineVhfGamepad.inf` (UMDF VHF source) and
`mouse_mapper/VibeshineVhfMouseMapper.inf` (KMDF mapper). Neither missing nor
untrusted mapper payloads may be treated as a functioning mouse device.

## Product identity and build

Pass the same explicitly allocated, product-owned **nonzero** VID and PID to
`tools/build-driver.ps1` as `-KeyboardMouseVendorId` and
`-KeyboardMouseProductId`. These become `LvgKeyboardMouseVendorId` and
`LvgKeyboardMouseProductId` for both WDK projects and the mapper INF. No identity
is allocated by this repository. Zero, borrowed controller IDs and a generic
mouse-class match are not release substitutes. The mapper INF targets only
`HID\VID_xxxx&PID_yyyy&REV_0100&Col02`; the source must retain that collection
order and version. Runtime binding additionally checks collection capabilities,
the source container and the mapper readiness handshake.

Use Windows 11 WDK 10.0.26100.0 with matching SDK and Visual Studio C++ tools.
`build-driver.ps1` builds the UMDF DLL, KMDF SYS and root-device setup tool, then
runs InfVerif on **both** INFs and generates one catalog in each directory.
The release configuration is x64; the build script also accepts ARM64 for
explicit architecture validation. Each package keeps its own architecture.

Local test mode signs the mapper SYS first, regenerates its CAT, then signs the
CAT with the same test certificate as the UMDF CAT and setup EXE. This does not
change Secure Boot, test-signing boot policy or certificate trust. A recipient
must explicitly prepare an authorized Windows test host. A trusted ordinary
code-signing certificate alone does not establish kernel load eligibility.

## Production signing boundary

The UMDF CAT and setup EXE still arrive unsigned and are deep-signed by the
consumer MSI SignPath request. **That request does not establish KMCS for the
KMDF mapper.** Submit the mapper INF/SYS/CAT through Microsoft attestation or
HLK signing. Supply the returned catalog for the exact built INF/SYS via
`verify-driver-package.ps1 -KernelSignedMapperCatalog <path>`; production
`build-release-package.ps1` requires this input. Both INF and SYS must pass
`signtool verify /kp /c <returned.cat> <member>` before the returned catalog is
staged. A catalog for another build fails even if its signature is trusted.
Do not modify the SYS, INF or returned CAT after verification.

### Preserved two-stage submission

A tag push runs **Prepare** once. With the owned identity variables
`VHF_MOUSE_VENDOR_ID` and `VHF_MOUSE_PRODUCT_ID` configured as decimal integers,
`build-release-package.ps1 -Stage Prepare -PreparationRunId <GitHub-run-id>`
builds the package, verifies fresh Inf2Cat evidence, and exports
`libvirtualgamepad-<version>-unsigned-run-<run-id>.zip` plus its SHA-256 sidecar.
The workflow uploads those exact bytes as
`unsigned-windows-<tag>-<run-id>`. The ZIP contains `package/` (including the
original mapper INF/SYS/CAT), original construction evidence, and
`preparation.json` with all original hashes, tag/source, DriverVer, protocol,
owned IDs and run ID. Download and retain this artifact. Submit its
`package/mouse_mapper/` bytes to the authorized Microsoft signing service.

After Microsoft returns the CAT, manually run **Release Windows package** with
four explicit inputs: the original tag, successful preparation run ID, original
unsigned ZIP SHA-256, and HTTPS URL of the returned CAT. The workflow checks the
prepare run's repository, workflow path, tag source SHA, event and successful
conclusion, then downloads that run's immutable artifact. **Resume never invokes
MSBuild or Inf2Cat.** It checks the explicitly pinned ZIP hash before extraction,
checks every original payload hash and metadata field against the tagged
checkout and owned identity configuration, and copies the preserved package.
Only the fresh evidence's old scratch-directory locator is rebased; no INF or
SYS byte changes. It imports the returned CAT only after `/kp /c` verifies exact
INF/SYS membership, then emits the final archive and actual release hashes.
The published evidence records the original ZIP hash and run ID.

The equivalent local resume interface is `build-release-package.ps1 -Stage
Resume -PreparedArchivePath <original.zip> -ExpectedPreparedArchiveSha256
<original-sha256> -PreparationRunId <original-run-id> -KernelSignedMapperCatalog
<returned.cat>`, with the same tag and owned VID/PID inputs. A separately rebuilt
SYS is never assumed reproducible. If Microsoft returns an embedded-signed SYS
with different bytes, this catalog-only import deliberately fails; a separate
reviewed full-package import would be required. The workflow does not configure
a Microsoft account or submit on the user's behalf. Missing owned IDs,
submission, original artifact/hash, or matching signed CAT blocks publication.

Both driver projects import `driver/MouseIdentity.props`. Direct MSBuild builds
reject missing/zero/hex-zero and out-of-range identities and normalize valid
numeric compiler definitions. The supported joint build supplies the same
inputs to both projects and INF rendering. It cannot prove that unrelated manual
builds used the same identity or that a numeric identity is actually allocated.

## Manifest, install and consumer handoff

New archives declare `features.mouse_mapper=1` and
`mouse_mapper.transport_version=1`. All mapper INF/SYS/CAT hashes appear in
`manifest.json`, fresh construction evidence and release metadata. Production
metadata records `kernel_signing=windows-kernel-policy`; unsigned candidate
metadata cannot pass production ingest. The mapper files are not in
`signed_downstream` and their hashes must remain exact after MSI deep signing.

The consumer checks kernel policy and exact catalog membership on ingest and
again through Windows driver trust policy before installation. It stages and
binds the mapper INF before starting the UMDF source, and preserves a mapper
restart requirement. An equal active parent does not skip mapper validation: the mapper is staged
independently, and equal stored mapper versions are retried for missing bindings.
An equal parent present only in Driver Store still reaches setup to create and
verify its missing root after mapper staging. Any strictly newer active or stored
parent fails before mutation: this candidate cannot attest a future parent's
mouse identity/transport, so a matching package at least as new is required. A
newer active/stored mapper also refuses downgrade before mutation. No older
bundled INF replaces or restarts a strictly newer parent. Final cleanup
removes the owned root device; mapper and
source Driver Store packages are removed only by the explicit removal option.
No class-wide mouse filter is installed or removed by these scripts.

Legacy immutable archives without the feature keep their original gamepad
contract. They do not acquire this fix from a source checkout. After a real
verified release exists, update consumer tag/source/DriverVer/protocol/archive
pins together using the actual published values. `SUNSHINE_REQUIRE_VHF_MOUSE_MAPPER_PACKAGE` defaults ON for bundled candidates
and host CI explicitly requires it during download, revalidation and packaging.
The old pinned archive now fails those release gates clearly; publishing and
verifying a real new producer artifact and refreshing its actual pins is
required before Windows candidate packaging can succeed. Explicit legacy
verification utilities remain compatible with old metadata.
There are no fabricated release pins in this change.
