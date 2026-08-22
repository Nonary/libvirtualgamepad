# Producer and SignPath contract

The libvirtualgamepad producer owns one build of the unsigned Windows x64
payload. Vibeshine and Vibepollo consume that same immutable pinned archive;
neither consumer compiles or regenerates any driver/package input.

## Producer release

For a clean tag matching `v0.1.0-beta.<positive-number>`, the producer:

1. Builds the x64 Release UMDF DLL and root-device setup tool.
2. Stages the DLL with its matching generated INF.
3. Runs mandatory `InfVerif` and `Inf2Cat` over the staging directory.
4. Verifies the exact tagged revision, DriverVer, protocol, x64 platform,
   payload hashes, five-file layout, absence of certificates, and
   `msi-request-signing` manifest channel. The CAT, DLL, and setup EXE must all
   report Authenticode `NotSigned` with no signer.
5. Creates the ZIP once, then writes its external SHA-256, release-lock, and
   evidence sidecars. The checksum names and hashes the archive; the lock and
   evidence identify the tag and target, DriverVer, protocol, archive and
   manifest hashes, exact layout, signing channel, and files signed downstream.
6. Requires the tag to remain a lightweight reference to the exact workflow
   revision before both draft creation and publication. It uploads and verifies
   the four files in the private draft, publishes the prerelease, and requires
   GitHub to report that exact release as immutable by both captured ID and tag.
   A failure before any publication attempt may delete only the captured
   mutable draft. Once publication is attempted, the release is preserved for
   inspection because the API outcome may be ambiguous and its immutable tag
   name cannot be reused. The producer does not sign, rebuild, or regenerate in
   the publish job and requires no signing secret.

Repository immutable releases are an operator-owned pre-tag prerequisite. The
operator enables and verifies the setting before pushing a release tag. The
workflow deliberately does not add a PAT or GitHub App secret for the
Administration-read setting endpoint; its scoped `GITHUB_TOKEN` is used only
for the release transaction, whose published responses must report
`immutable: true`.

The ZIP contains only:

```text
driver/VibeshineVhfGamepad.inf
driver/VibeshineVhfGamepad.dll
driver/VibeshineVhfGamepad.cat
tools/VibeshineVhfGamepadDeviceSetup.exe
manifest.json
```

Sidecars stay outside the ZIP to avoid a circular archive hash.

An unsigned catalog cannot use SignTool's signed-catalog membership check. The
producer therefore requires evidence emitted immediately after mandatory
successful `Inf2Cat` over the fresh package directory. It binds the exact INF,
DLL, and CAT hashes and is recorded as the `fresh-inf2cat` membership basis in
the manifest, release lock, and evidence. Signed consumer verification still
uses SignTool to prove both the catalog signature and membership.

## Consumer signing

Each Vibeshine or Vibepollo MSI pipeline downloads the same pinned producer
asset and validates the archive checksum, producer release lock, manifest hash,
source revision, DriverVer, protocol, platform, exact layout, payload hashes,
signing channel, and downstream file list before extraction into its packaging
staging area. A consumer adds its own local release lock; it does not modify the
producer ZIP or rebuild any producer file.

The consumer's MSI SignPath request has two independent artifacts:

1. Sign `VibeshineVhfGamepad.cat` with
   `vhf-gamepad-catalog.artifact-config.xml`.
2. Sign `VibeshineVhfGamepadDeviceSetup.exe` with
   `vhf-gamepad-device-setup.artifact-config.xml`.
3. Restore those two signed files, verify the catalog signature and its binding
   to the unchanged INF/DLL, and verify the setup tool's embedded signature.

The producer manifest intentionally hashes the unsigned catalog and setup tool.
Those two hashes become stale after the authorized downstream signing step; no
other producer payload hash may change.

Do not add the driver DLL to a generic MSI deep-signing request. Re-signing the
DLL after `Inf2Cat` changes its catalog hash. If a future UMDF load test proves
that the DLL must be embedded-signed, do that before `Inf2Cat` and use a
separate reviewed SignPath contract for it.

The setup tool is not catalog-bound, so it must be Authenticode-signed as a PE
in the consuming MSI's request. Do not sign or rewrite it again after that
request.

## Local test packages

`tools/build-driver.ps1` defaults to `-SigningMode LocalTest`, mirroring
Vibeshine's virtual-display package refresh flow. After `Inf2Cat`, it creates
or reuses a private test certificate in `CurrentUser\My`, signs the catalog and
the setup tool, and exports only the public `driver/VibeshineVhfGamepad.cer`. The
elevated `tools/trust-test-certificate.ps1` explicitly trusts that public
certificate in the test host's `LocalMachine\Root` and
`LocalMachine\TrustedPublisher` stores. This establishes certificate trust; it
does not change Windows test-signing policy.

Release automation uses `-SigningMode Release`, which produces no `.cer` and
leaves the catalog and setup tool unsigned for their consumer-owned SignPath
request. A local certificate, its private key, and any package produced in
`LocalTest` mode are never release inputs.
