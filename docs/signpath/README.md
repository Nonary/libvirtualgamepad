# SignPath contract

Production signing has two independent artifacts:

1. Build one architecture's UMDF DLL.
2. Stage that DLL with the matching generated INF.
3. Run `InfVerif` and `Inf2Cat` over the staging directory.
4. Submit `VibeshineVhfGamepad.cat` with
   `vhf-gamepad-catalog.artifact-config.xml`.
5. Submit `VibeshineVhfGamepadDeviceSetup.exe` with
   `vhf-gamepad-device-setup.artifact-config.xml`.
6. Restore both signed artifacts, verify the catalog against the final INF and
   DLL and verify the setup tool's embedded signature, then publish the
   architecture-specific archive and its manifest.

Do not add the driver DLL to a generic MSI deep-signing request. Re-signing the
DLL after `Inf2Cat` changes its catalog hash. If a future UMDF load test proves
that the DLL must be embedded-signed, do that before step 3 and use a separate
reviewed SignPath contract for it.

The setup tool is not catalog-bound, so it must be Authenticode-signed as a PE.
Do not re-sign it in a downstream MSI: the release manifest hashes the setup
tool as well as the catalog-bound DLL, so either rewrite would invalidate the
immutable package contract.

## Local test packages

`tools/build-driver.ps1` defaults to `-SigningMode LocalTest`, mirroring
Vibeshine's virtual-display package refresh flow. After `Inf2Cat`, it creates
or reuses a private test certificate in `CurrentUser\My`, signs the catalog and
the setup tool, and exports only the public `driver/VibeshineVhfGamepad.cer`. The
elevated `tools/trust-test-certificate.ps1` explicitly trusts that public
certificate in the test host's `LocalMachine\Root` and
`LocalMachine\TrustedPublisher` stores. This establishes certificate trust; it
does not change Windows test-signing policy.

Release automation must use `-SigningMode Release`, which produces no `.cer`
and leaves the catalog and setup tool for their separate SignPath requests. A
local certificate, its private key, and any package produced in `LocalTest`
mode are never release inputs.
