<!-- SPDX-License-Identifier: MIT -->
# Pixel Fold (felix) first light

Status (2026-10-13): slot B boots and SSH comes up at `root@172.16.42.1`.
The cover shows the Linux console after the cover controller's trigger is
unmasked (`DECON1` `TRIG_CON` `0x1c241030` written with `0x3061`). The inner
controller stays masked. The inner panel keeps the bootloader logo, including
while the phone is folded, because this kernel does not turn that panel off.
Do not reset it or program its DSI. GrapheneOS is still on slot A. Slot B
has not been marked successful. Kernel 7.3.0-rc6.

## Facts

The stock capture and the cover-console boot are in [NOTES.md](NOTES.md).

## What booted

The local board file enables the USB controller and the cover framebuffer
the bootloader is already scanning, and it relies on the SoC tree for the
cluster watchdogs. Both panel nodes, both touch controllers, the GPU, and
UFS stay disabled. That board file is not in this repository. It will be
proposed against linux-omarchy-mobile, branch `omamobile`.

The shared init writes only this device's `SIMPLEFB_TRIG_CON`, the cover
trigger. It does not write the Pixel 7 Pro register `0x1c240030`, and
`BRINGUP_PROBE` is unset so it does not read the display DMA registers.
`device/felix/device.conf` is what `bin/omamobile` reads. The default
device stays cheetah. A Fold image is `DEVICE=felix`.

## First boot

Phone folded. Flash only slot B. Slot A stays GrapheneOS. Expect SSH at
`root@172.16.42.1` and a Linux console on the cover. The inner panel keeps
the bootloader logo, through the hinge crack as well. Nothing in the tree
has its reset pin or its rails.

Stop if the inner picture changes from that logo, if the cover brightness
changes on its own, or if the phone does not come back to slot A after a
bad boot.
Hold volume down and power for Fastboot Mode, then
`fastboot --set-active=a`.
