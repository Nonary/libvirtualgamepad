# pid.codes product ID request

**Status:** not submitted. Submitting means opening a pull request against a
third-party public repository under Chase's GitHub account, so it needs him.

## Why this is not optional

The generic profiles (`generic_hid`, `generic_pid`) present `1209:0001`.
pid.codes designates that as the Test PID, and the wording is not advisory:

> This PID is reserved for use in private testing. Anyone may assign it to their
> device while they're testing in-house, but it MUST NOT be used on any device
> that will be redistributed, sold, or manufactured.

The VHF driver has not shipped yet — this work is on `vhf_gamepad_large`, not
promoted — so nothing is in violation today. It would be the moment a build
carrying these profiles reaches users. Fix it before that, not after.

pid.codes also asks that source referencing the test PID warn about it. The
comment at `k_vibeshine_product_id` in `driver/src/profile.cpp` already does.

The console profiles are a separate matter and are unaffected. They present the
real vendor identities (`045E` Microsoft, `054C` Sony, `057E` Nintendo) because
the `xinputhid` filter and the host-side drivers that recognise those pads match
on exactly those IDs. A different identity would not reach them, which is the
same reason every virtual gamepad driver does this.

## Eligibility

Both hard requirements are met, verified 2026-08-19:

- **Public source repository** — <https://github.com/Nonary/libvirtualgamepad>
  is public.
- **Recognised open source licence** — MIT.

The real risk is a softer one. pid.codes notes that software-only projects
"may face additional scrutiny", and asks "for further justification as to why
you need a PID". This device is *entirely* virtual: there is no board, no
firmware, and it never appears on a real USB bus. Expect to be asked, and answer
with the justification below rather than treating a query as a refusal.

### Justification to offer if asked

The driver presents a HID device to Windows, and Windows identifies it by
VID/PID like any other. Applications key per-device settings to that pair —
per-game bindings, Steam controller configuration, saved DirectInput
calibration — so two different virtual devices sharing one ID collide in a way
the user sees. The software is redistributed to end users, which is precisely
the case the Test PID excludes. We are not asking for an identifier for a
product we might build; we are asking because we ship one that Windows already
demands an identity for.

## How to submit

1. Fork <https://github.com/pidcodes/pidcodes.github.com>.
2. Add `org/<owner>/index.md` if that owner has no entry yet:

   ```
   ---
   layout: org
   title: Nonary
   ---
   Maintainer of Vibeshine, an open source game streaming host for Windows.
   ```

3. Pick an unallocated PID. The `0xxx` and `1xxx` ranges are reserved and must
   not be requested. As of 2026-08-19, `1209/5000` is taken and `5001` upward
   were free — re-check before submitting, since this moves.
4. Add `1209/<pid>/index.md`:

   ```
   ---
   layout: pid
   title: Vibeshine Virtual Gamepad
   owner: Nonary
   license: MIT
   site: https://github.com/Nonary/vibeshine
   source: https://github.com/Nonary/libvirtualgamepad
   ---
   Virtual HID game controller presented by the Vibeshine game streaming host's
   UMDF/VHF driver, so that a streamed game sees a controller on the host. The
   device exists only in software and ships no hardware.
   ```

5. Open a pull request with a descriptive commit message.

## What to change once an ID is allocated

`driver/src/profile.cpp`: `k_vibeshine_product_id`, replacing the `0x0001`
placeholder, and drop the warning comment above it. Nothing else references it.

## If the request is declined

The generic profiles are the only thing affected, and they are already
unreachable: Sunshine offers no option that selects one, and the automatic
ladder reaches them only when no console profile is offered. They exist as a
version-skew fallback for a new host talking to an older driver.

So the fallback position is simply to stop offering them, which removes the
prohibited identifier along with them. That costs the fallback — a user whose
driver is older than their host would get no controller instead of a plain one —
which is worth something, but not worth shipping an identifier we were told not
to ship. Do not respond by inventing a different unowned VID; that is squatting,
and unlike the console profiles there is no technical necessity to justify it.
