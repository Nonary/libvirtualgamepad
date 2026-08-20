# pid.codes product ID request — draft for Chase to submit

**Status:** not submitted. Requesting an allocation means opening a pull request
against a third-party public repository under your name, so it needs you rather
than me.

## Why

The generic profiles (`generic_hid`, `generic_pid`) currently ship with
`1209:0001`. pid.codes documents that ID as the prototype/test ID and asks that
it not be used on anything distributed. Two devices sharing it on one machine
collide in anything that keys on VID:PID — per-game device bindings, Steam
controller configuration, saved DirectInput calibration.

The console profiles are unaffected. They present the real vendor identities
(`045E` Microsoft, `054C` Sony, `057E` Nintendo), which is deliberate: the
filter and host-side driver matching those profiles depend on are keyed to those
IDs, and a different identity would not reach them.

## How urgent

Low, but not zero. Neither generic profile is user-selectable, and the automatic
ladder reaches them only when no console profile is offered — which on a current
driver never happens. The realistic exposure is a version-skew case: a newer
Sunshine talking to an older driver that offers nothing else.

Worth starting early anyway, because allocation turnaround is out of our hands.

## Where to submit

<https://github.com/pidcodes/pidcodes.github.com> — add a directory under
`1209/` for the requested ID with an `index.md` describing the product.

## Suggested content

```yaml
---
name: Vibeshine Virtual Gamepad
description: >
  Virtual HID game controller presented by the Vibeshine game streaming host's
  UMDF/VHF driver. The device exists only in software; it carries no firmware
  and ships no hardware.
---
```

## What to change once an ID is allocated

`driver/src/profile.cpp`: `k_vibeshine_product_id`, replacing the `0x0001`
placeholder. The comment there already flags it as a test ID pending
allocation. Nothing else references it.
