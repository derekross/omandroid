<!-- SPDX-License-Identifier: MIT -->
# Pixel Fold (felix) bring-up notes

Findings from real hardware, newest first. [BRINGUP-M1.md](BRINGUP-M1.md) is the first-light record.

## 2026-10-13: cover console

- **Slot B boots folded.** USB networking and SSH at `root@172.16.42.1`. Both cluster watchdogs are stopped. Slot B is not marked successful. Slot A stays GrapheneOS.
- **The cover console is the cover controller only.** ABL leaves DECON1 `TRIG_CON` (`0x1c241030`) at `0x3070`, which masks the hardware trigger, so the cover keeps the bootloader logo. Writing `0x3061` clears `HW_TRIG_MASK_DECON` and sets `HW_TRIG_EN`. That pair is `SIMPLEFB_TRIG_CON` in `device.conf`. The shared init writes it only when the board's first compatible string is this descriptor's `COMPATIBLE`, and the cover shows the Linux console. Kernel 7.3.0-rc6.
- **DECON0 `TRIG_CON` (`0x1c240030`) is the inner panel on this phone.** That is the address the Pixel 7 Pro init writes. It was not written. The inner frame count stayed frozen, and the inner panel kept the bootloader logo, including while folded. Do not reset it or program its DSI.
- **The board file is local.** It enables the USB controller and the cover framebuffer the bootloader is already scanning, and it leaves both panel nodes, both touch controllers, the GPU, and UFS disabled. It will be proposed separately against `linux-omarchy-mobile` branch `omamobile`.
- **UFS stays disabled** until this phone's read-only guard has been checked and there is a read-only backup of `efs`, `efs_backup`, `modem_userdata`, `persist`, and `modem_a`/`modem_b`.

## 2026-10-13: stock facts

| Fact | Value | Source |
| --- | --- | --- |
| SoC | GS201, MP1.0, package mode 0, SoC rev 2. Same B0 match as cheetah (`soc_id` 0x09855001) | [V] `getprop` ro.boot.hw.soc.pkg_mode, ro.boot.hw.soc.rev, ro.boot.hardware.revision |
| Board id | 0x30606, rev 0x10000, dtbo index 10 | [V] `ro.boot.cdt_hwid`, `ro.boot.dtbo_idx` |
| Bootloader | felix-17.0-15902555, unchanged by the unlock | [V] `fastboot getvar version-bootloader` |
| Cover panel | 1080×2092, 120 Hz, DSI port 1. Active while folded (device state 0) | [V] `dumpsys display` |
| Inner panel | 2208×1840, 120 Hz, DSI port 0. Not described in the board file | [V] `dumpsys display`; Google `gs201-felix-display.dtsi` |
| Slots after the GrapheneOS reinstall | A successful and current. B bootable, not yet successful, not unbootable | [V] `fastboot getvar` |

The framebuffer address is not copied from the Pixel 7 Pro. On that phone ABL injects the splash carve-out, and the 7 Pro tree names `0xfac00000`. Using that address here would `no-map` the wrong RAM if the Fold's splash lives elsewhere.
