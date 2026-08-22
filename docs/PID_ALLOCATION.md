# pid.codes product ID request

**Status:** not submitted. The protocol enum values remain reserved, but both
generic profiles are unavailable until an allocation is accepted. Submitting
means opening a pull request against a third-party public repository under
Chase's GitHub account, so it needs him.

## Why this is not optional

pid.codes designates `1209:0001` as the Test PID, and the wording is not
advisory:

> This PID is reserved for use in private testing. Anyone may assign it to their
> device while they're testing in-house, but it MUST NOT be used on any device
> that will be redistributed, sold, or manufactured.

The public driver has no production profile definition carrying that identity.
`find_profile()` refuses `generic_hid` and `generic_pid`, so the advertised mask
omits them and both normal and raw create requests fail. The protocol enum
numbers and generic report encoders remain stable for a future accepted
allocation. Source references in the DirectInput probes are explicitly private
test only and must never become release inputs.

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

Add reviewed generic profile definitions in `driver/src/profile.cpp` using the
accepted VID/PID, then enable those definitions in `find_profile()`. Update the
focused profile-mask and identity tests in the same change. Keep the protocol
enum numeric values unchanged.

## If the request is declined

The current safe fallback is already in force: keep both generic profiles
unavailable. A host must choose one of the advertised console profiles or fail
the request; it must not create a device with the private-test identity. Do not
respond by inventing a different unowned VID; that is squatting, and unlike the
console profiles there is no technical necessity to justify it.
