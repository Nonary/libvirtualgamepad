# Power-transition controller cleanup

Adapted from illixion's contribution in [PR #4](https://github.com/Nonary/libvirtualgamepad/pull/4), against current main's non-power-managed filter queue. That queue setting is retained, not introduced by this change.

D0 exit stops admission, detaches VHF handles, drains callbacks with synchronous VhfDelete, stops the PID timer and leaves an owner-bound lost slot. Existing input/feedback requests report device removed. A different owner cannot steal that slot. Closing or explicitly destroying it releases ownership; its original owner may also recreate it after D0 entry.

Creation takes one Configuration Manager snapshot before reusing the stable instance ID. A present child, failed enumeration, allocation failure, resized list or malformed result returns busy. There is no driver-side polling or misleading two-second timeout. The caller can retry after resume with its own bounded backoff. Power, ownership and slot generation are rechecked after enumeration, without holding the lifetime lock while querying PnP. This is a bound on driver retry work, not a time limit on Windows API calls or VhfDelete.

## Portable verification

The existing test_vhf_lifecycle target executes the shipping driver body against modeled WDF/VHF and Configuration Manager boundaries. Tests exercise all profiles across D0 exit/entry, callback buffer lifetime, owner-preserving loss, duplicate teardown, file close, child presence, enumeration failures, power-off during enumeration and stale slot snapshots. It does not install or load a driver. The initialization test invokes registered D0 callbacks in addition to hardware preparation.

## Optional native probe

On Windows, build driver/tests with CMake; probe_sleep is a manual executable, never a CTest. It requires an already installed compatible driver, S3 and wake timers. Running it intentionally suspends the machine and can reproduce a system hang or bugcheck on affected drivers. Save work and use a disposable test machine with physical recovery access. Do not run unattended on a reporter's machine.

Usage: probe_sleep awake|none|idle|churn|aware [wake-seconds] [log-file]. Start with awake. Other modes require elevation for the sleep privilege. The churn/aware check now requires a successful create, input submission and destroy cycle that started after a resume event; pre-sleep cycles cannot pass it.

Portable tests cannot validate WDK compilation, actual D0/IRP ordering, native probe compilation, S3 behavior or a Vhf.sys watchdog crash. These remain explicit native validation requirements; this source change is not proof that the reported kernel sleep crash is fixed.
