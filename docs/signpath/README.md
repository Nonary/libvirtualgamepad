# SignPath contract

Production signing operates on the final catalog only:

1. Build one architecture's UMDF DLL.
2. Stage that DLL with the matching generated INF.
3. Run `InfVerif` and `Inf2Cat` over the staging directory.
4. Submit `VibeshineVhfGamepad.cat` with
   `vhf-gamepad-catalog.artifact-config.xml`.
5. Restore the signed catalog, verify it against the final INF and DLL, then
   publish the architecture-specific archive and its manifest.

Do not add the driver DLL to a generic MSI deep-signing request. Re-signing the
DLL after `Inf2Cat` changes its catalog hash. If a future UMDF load test proves
that the DLL must be embedded-signed, do that before step 3 and use a separate
reviewed SignPath contract for it.
