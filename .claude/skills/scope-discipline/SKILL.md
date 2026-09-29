---
name: scope-discipline
description: Feature-scope discipline for the StockStick firmware (a dedicated investor companion device, not an e-reader). Use when adding a feature, activity, lib, setting, or dependency, or when a request would grow the firmware's surface. Covers the SCOPE.md test, the RAM and OTA-partition budget gate, preferring existing mechanisms, and how to push back on out-of-scope asks.
---

# Scope Discipline

The mission: StockStick content, synchronization and device operations on
constrained hardware. `SCOPE.md` is the source of truth for what is in and out.
Read it before adding surface.

## The gate

Before adding a feature, activity, lib, setting, or dependency, answer in order:

1. **Is it in `SCOPE.md`?** The ebook reader and upstream ecosystem connectors
   (web file transfer, OPDS, KOReader, Calibre) were removed on purpose; do not
   bring them back. If it is out, say so and stop.
2. **Does it serve StockStick content, sync, interaction or device operations?**
   "Nice to have" for another use case is out.
3. **What does it cost?** Steady-state RAM, largest free block (TLS and BLE need
   it), and flash inside the fixed 6,553,600-byte OTA slot. Measure with the
   build's size check and `firmware_size_history.py`, do not guess.
4. **Can it be done with no new code?** Prefer an existing activity, setting or
   the server side (the mini program renders pixels; the firmware does not lay
   out text).

If a request fails the gate, push back with the specific reason and the
`SCOPE.md` basis, and offer the in-scope alternative.

## Settings are not free

A new setting is a field to persist, migrate, validate, translate and render.
Persisted enum values keep their numbers forever; retired values fold to a
device equivalent on load.

## Self-review

- [ ] Checked against `SCOPE.md`; not on the out-of-scope list.
- [ ] Stated the concrete StockStick benefit.
- [ ] Named the RAM/flash cost (measured) and the remaining OTA headroom.
- [ ] Checked whether an existing activity/setting/server feature already covers it.
- [ ] New setting (if any) is justified by a real user need.
