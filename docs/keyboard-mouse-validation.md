# Keyboard/mouse source validation record

Scope: optional producer prerequisite for Vibepollo #602, stacked on
libvirtualgamepad PR8. Initial base was
`fd67a37554f17fce3c6ad586f6acb20f7a48cb8d`; the dependency was then advanced to
`3dc2bb48667ae898a337691828f07a6e0ab216ba`, incorporating PR8's independent
one-line Windows probe wide-string repair. No probe or PR8 branch was edited by
this producer adaptation. This record is source/portable evidence, not native
qualification or independent-review clearance.

## Executed checks (2026-10-22)

- GCC 14.2, C++20, CMake 4.4.4 on Linux: all 9 CTests pass in Debug and Release.
- Actual driver/client KM transport: 3,023 assertions pass, including the
  power-specific subset's 996 assertions. Pure producer core: 384 assertions.
  The existing controller lifecycle test passes with the added early-PnP
  cancellation expectations and default-OFF IOCTL refusal checks.
- ASan + UBSan: all 9 CTests pass with `ASAN_OPTIONS=detect_leaks=0`.
  LeakSanitizer was attempted but cannot run under this executor's ptrace
  boundary; leak checking is not claimed.
- Focused controller lifecycle/initialization and KM core/transport targets
  compile and pass with `-Wall -Wextra -Werror`, retaining the existing targeted
  suppressions for the driver pool-tag literal and trivially-copyable reset.
  An extra strict-warning aggregate build did not pass because unchanged
  `test_pid_descriptor.cpp:1193` mixes enum/non-enum conditional operands.
  The normal aggregate build/CTest pass does not hide or fix that unrelated
  extra-warning failure.
- Header guard compilation accepts default-OFF and the explicit synthetic
  in-process test configuration; source opt-in without acknowledgement, or
  acknowledgement without an identity, fails compilation as intended.
- Release controller lifecycle, KM source transport and KM power lifecycle each
  pass 50 consecutive CTest runs (150 executions total).
- `git diff --check` passes.

Reproduce portable checks without loading/installing anything:

```sh
cmake -S driver/tests -B build/km-contracts -DCMAKE_BUILD_TYPE=Debug
cmake --build build/km-contracts -j4
ctest --test-dir build/km-contracts --output-on-failure

cmake -S driver/tests -B build/km-sanitized -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build build/km-sanitized -j4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/km-sanitized --output-on-failure
```

## Behavioral negative controls

`python3 driver/tests/run_keyboard_mouse_negative_controls.py` builds the
unmodified actual-body fixture first, then injects each defect separately into
a disposable source copy. It never edits the checkout under test. Each mutation
must compile successfully and finish with assertion failures; compilation errors,
crashes and timeouts are not counted as valid rejection evidence.

All six compiled mutations were rejected:

1. Remove KM power admission checks: 24 failed assertions.
2. Omit KM D0 retirement/deletion: 6 failed assertions in the minimal mixed-child
   retirement probe.
3. Remove the post-enumeration generation guard: 7 failed assertions.
4. Accept oversized raw requests: 3 failed assertions.
5. Expose unvalidated transport busy as retry-safe: 1 failed assertion.
6. Reset retained report storage before VhfDelete: 48 failed assertions.

These are tests of executed production bodies and observable status, ownership,
submission, retained bytes and deletion ordering. Source-string matching is used
only to inject deliberate mutations, not as a substitute for runtime behavior.

## Still unverified

No Windows SDK/WDK, native UMDF/VhfUm build, hardware, VM, real suspend/hibernate,
Driver Verifier, callback scheduler, held-input removal, signed package, accepted
VID/PID, native absolute binding or end-to-end host backend was available/run.
The fixture assumes ready-callback entry has quiesced by VhfDelete(TRUE) return;
that exact VhfUm contract remains unqualified. Synchronous native calls can block
indefinitely. CI and the separate independent review passes must inspect the
published head and dependency base. No release, installation, security-setting
change, signing, migration, merge, issue closure, or nightly claim is made.
