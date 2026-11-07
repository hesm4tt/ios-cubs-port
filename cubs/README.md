# ios-cubs: iOS 16.5 (HackiOS XNU) on the Pixel 11 "cubs"

Work in progress port of HackiOS to a Google Pixel 11 (codename `cubs`). Based on `oosexgang/hakios-xnu-kernel`
(upstream history kept under the `upstream` remote); see the upstream LICENSE files, which still apply.

## Status (2026-10-30)
- XNU boots natively to userland from a Linux/Android "hand-over": an unsigned kernel module stages the XNU payload
  into reserved RAM and jumps into it at EL1 with the MMU off, leaving the display as Linux configured it.
- Key fix: Linux leaves `ICC_CTLR_EL1.EOImode=1`; XNU needs 0 or timer interrupts never deactivate.
- launchd currently calls `reboot(RB_HALT)` ~70 s in (cause under investigation). No display driver yet, so the lock
  screen has **not** been reached.

## Layout
- `cubs/handover/` kernel module (`cubs_stage.c`), stub/trampoline (`blobs.S`), module metadata, build script.
- `cubs/pcwork/` PC-side drivers (`pcrun.py`, `readlog.py`, ...) that push and run the experiments over adb.
- `cubs/docs/` running checklist, plan and notes. `cubs/legacy-device/` early draft device profile (predates upstream's current `devices/` layout).

## Not included (on purpose)
Vendor firmware, boot images, device trees, IPSW content, trace dumps, XNU payload/kernel binaries and device
serials. Building needs those from your own device and IPSW.
