<!-- SPDX-License-Identifier: MIT -->
# Contributing to Omarchy Mobile

Omarchy Mobile runs native Omarchy on Google Pixel phones: Arch Linux ARM, Hyprland and Omarchy's own packages on a mainline Linux kernel, with no Android underneath. It works today on the Pixel 7 Pro (`cheetah`, Google Tensor G2 / `gs201`). We want help with two things: bringing up more Pixels, and getting our device-tree and driver work merged upstream.

- **Supported devices and their status:** [docs/DEVICES.md](docs/DEVICES.md)
- **What's missing and the order we plan to tackle it:** [docs/ROADMAP.md](docs/ROADMAP.md)
- **AI coding agents:** read [AGENTS.md](AGENTS.md) as well. It has the rules and the gotchas we learned the hard way.
- **What has been learned on real hardware:** [device/cheetah/NOTES.md](device/cheetah/NOTES.md), newest first.

## Contents

1. [Goals and principles](#1-goals-and-principles)
2. [What you need](#2-what-you-need)
3. [The safety model (read this first)](#3-the-safety-model-read-this-first)
4. [The development loop](#4-the-development-loop)
5. [Bringing up a new device](#5-bringing-up-a-new-device)
6. [Submitting changes](#6-submitting-changes)
7. [Licensing and vendor firmware](#7-licensing-and-vendor-firmware)

---

## 1. Goals and principles

- **Omarchy-native.** The phone runs Omarchy's own packages, shell and update path (`omarchy update`). Phone-specific behaviour lives in Omarchy Mobile packages (`omamobile-shell`, `omamobile-device-<codename>`) that sit beside Omarchy's, as `omarchy-mac` does for Apple Silicon. Changes Omarchy itself needs go upstream to Omarchy ([`patches/omarchy/`](patches/omarchy) holds them while they wait). Avoid one-off local hacks.
- **Upstream-able changes.** The kernel is mainline plus one commit per logical change, written the way upstream wants it: bindings, then the driver, then the DTS. Mesa and Hyprland changes are kept as patch files with their upstream merge-request text (see [`packages/mesa/UPSTREAM.md`](packages/mesa/UPSTREAM.md)). Any change we can't send upstream needs a good reason, written down.
- **No Google Play services**, and no Google apps. Android apps run in Waydroid. Where Google's proprietary userspace is needed (GNSS, fingerprint), it runs in a small chroot built from the user's own factory image, and only for the job it does.
- **Safety first.** Every Pixel holds calibration data that can't be replaced: IMEI, radio NV, sensor and display calibration. Bricking a phone or wiping that data is the worst thing this project can do. The [safety model](#3-the-safety-model-read-this-first) comes before speed, every time.
- **Evidence over guesses.** The plans mark each claim as checked ([V], with a source or a capture) or inferred ([I], to be checked on the device). Results from the phone go into the plan and into `NOTES.md` with the kernel version that produced them.

## 2. What you need

### Hardware

- **A supported or candidate Pixel with an unlockable bootloader.** "OEM unlocking" must be allowed in Android's developer options. Carrier-locked phones often can't unlock. Unlocking (`fastboot flashing unlock`) wipes the phone.
- **The factory image for exactly the build on your phone**, from [developers.google.com/android/images](https://developers.google.com/android/images). Our scripts pull the firmware each driver needs from that image (§7). Keep it: it's also your restore path.
- **A USB-C cable and a Linux laptop.** USB is the only link to the phone during bring-up: networking (CDC NCM, the phone is `172.16.42.1`) and a serial console (ACM).
- Patience, and a phone you can afford to have out of action for a while.

### Laptop

- `docker`: kernel builds run in the [`docker/Dockerfile`](docker/Dockerfile) environment (Ubuntu 24.04).
- `adb`, `fastboot` (`android-tools`), `lz4`, `zstd`, `cpio`, `python3`, `ssh`, plus `unzip`, `debugfs` (e2fsprogs), `simg2img`/`lpunpack` and `erofs-utils` for firmware extraction.
- **Android's prebuilt clang and the AOSP boot-image tools.** `bin/omamobile` takes clang from `CLANG_DIR`, and `mkbootimg` and `mkdtboimg.py` from `SRC_DIR` (`system/tools/mkbootimg`, `system/libufdt`). Today these come from a LineageOS checkout (`bin/lineage-ref sync`, about 400 GB). A smaller setup also works: the AOSP `platform/prebuilts/clang/host/linux-x86`, `platform/system/tools/mkbootimg` and `platform/system/libufdt` projects under `~/android`, with `CLANG_DIR` and `SRC_DIR` set in `omamobile.local.conf`. The initramfs build links against that clang's Android compiler-rt builtins. A lighter toolchain setup would make a good PR.
- **The kernel tree.** Clone [derekross/linux-omarchy-mobile](https://github.com/derekross/linux-omarchy-mobile) to `~/android/mainline/linux`, or set `KERNEL_DIR`. Branch `omamobile-int` is what the phone runs today (every subsystem merged, work in progress included, as one snapshot commit); branch `omamobile` holds the curated, upstream-ready commits and lags behind it. Build `omamobile-int` to match this repository's packages and overlays; base your own kernel work on `omamobile`.
- **Google's GPL kernel for your device**, as a reference for registers, clocks and power sequences (manifests at `android.googlesource.com/kernel/manifest`, branches named `android-gs-<family>-…`). The postmarketOS Tensor kernel is a useful second reference.
- **The host side of USB networking:** give the laptop `172.16.42.2/24` on the phone's USB network interface. A NetworkManager profile with a manual address works (see "Host setup" in [NOTES.md](device/cheetah/NOTES.md)).

Shared settings live in [`omamobile.conf`](omamobile.conf). Override them in `omamobile.local.conf`, which is gitignored.

## 3. The safety model (read this first)

Every rule here exists because breaking it either bricks a phone or loses data no one can restore. If a step in a plan conflicts with this section, this section wins.

### 3.1 Stock Android stays on slot A

- Pixels have A/B slots. **Omarchy Mobile only ever lives on slot B.** Slot A keeps the stock factory build, so the phone always has a known-good system to fall back to and a way back to `fastboot` (`adb reboot bootloader`).
- **We flash only these partitions:** the device's boot images on slot B (its descriptor's `BOOT_IMAGES`: `boot_b`, `vendor_boot_b` and `dtbo_b`, plus `init_boot_b` and `vendor_kernel_boot_b` where the phone has them), and `userdata`, which holds the Arch root.
- **Never flash** `bootloader`, `abl`, `bl1`/`bl2`/`bl31`, `pbl`, `radio`/`modem`, `devinfo`, `persist`, `efs*`, `modem_userdata`, `vbmeta`, or anything without an explicit `_b` suffix. `fastboot flash boot …` without a suffix writes whichever slot is active.
- **Never relock the bootloader** (`fastboot flashing lock`) while any non-stock image is on the phone.
- **Never flash a factory image older than the one installed.** Pixels enforce bootloader anti-rollback.
- Before your first flash, check that both slots carry the same bootloader version (`fastboot getvar all`). If they don't, stop and ask in an issue.

### 3.2 The read-only storage guard

UFS holds the per-device calibration. The initramfs ([`initramfs/init`](initramfs/init)) guards it before anything else runs:

- It sets every SCSI disk and partition read-only (`blockdev --setro`) as soon as it appears, and keeps a watcher running for the life of PID 1.
- The only exceptions are an allowlist of GPT partition names: the device's `BOOT_IMAGES` on slot B, and `userdata`.
- It deletes the whole-disk node of the main LUN (`/dev/sda`), so nothing can write across partitions. The boot LUNs stay read-only at the disk level.
- In Arch, [`10-omamobile-storage.rules`](packages/omamobile-device-cheetah/vendor/udev/rules.d/10-omamobile-storage.rules) keeps the whole-disk node unusable and hides the Android partitions from desktop tools.
- The one write outside the allowlist is the boot-success marker (`omamobile-boot-success`). It opens exactly `devinfo` for a single verified write of the slot flag bytes, then sets it read-only again.

**A new device gets this guard before its UFS driver is ever enabled.** Check the device's LUN layout and partition names first, because the allowlist works by name.

### 3.3 Back up and checksum before anything risky

Before you touch radios, the PMIC, the AoC or Trusty, take a **read-only** copy of `efs`, `efs_backup`, `modem_userdata`, `persist`, `modem_a`/`modem_b` and `devinfo`. Better still, copy every partition except `userdata` and `super`.

- Read them with `dd` over SSH from the guarded initramfs, or from a rooted reference Android.
- Store them **outside the repository** (we use `~/android/reference/`) with a `SHA256SUMS` file.
- Re-check the checksums before radio work. [MODEM-PLAN.md](device/cheetah/MODEM-PLAN.md) M0.1 shows how; it finds each partition by `PARTNAME` in `/sys/class/block/*/uevent`, because the Arch root has no `/dev/disk/by-partlabel` links.
- **Take a fresh backup after any stock Android boot**, including a fallback to slot A. Stock's modem stack writes `efs`, `persist` and `modem_userdata`, so the old checksums no longer match, and a later mismatch would no longer tell you whether something of ours wrote there.
- These files contain your IMEI and serial numbers. They must never be committed, posted or attached to an issue.

### 3.4 Hangs: dead-man switches, watchdogs and recovery

Bring-up steps hang phones. Plan for it before you start:

- **Capture the log first.** The stage tools (`tools/wifi-w1-capture`, `acpm-stage`, `aoc-stage`, `fingerprint-capture`) open one SSH connection that streams `/dev/kmsg` into `logs/` **before** a second connection touches the hardware. The last line before a hang is then already on the laptop.
- **Arm the hardware watchdog for risky loads.** Pass `--watchdog`: an s3c2410 watchdog (about 15 s) with a feeder loop. A full hang then ends in a warm reset by itself, and pstore keeps the console (`--pstore` saves it afterwards).
- **Use dead-man switches for anything that can cut the USB link** (USB-C, firewall, network changes). The step runs detached on the phone and undoes itself unless the laptop confirms over a **new** SSH login. If even the undo doesn't bring USB back, the watchdog resets the phone into the known-good setup. See [`tools/usbc-deadman`](tools/usbc-deadman). The quick version for a config change is `systemd-run --unit=x-deadman --on-active=90 <undo command>`: test with a fresh connection, then stop the timer.
- **Use hang markers for anything that runs at boot.** Write and `sync` a marker before touching the hardware, and remove it once the step has finished. If the next boot finds the marker, it skips the step. See [`omamobile-wifi-start`](packages/omamobile-device-cheetah/bin/omamobile-wifi-start) and [`omamobile-aoc-start`](packages/omamobile-device-cheetah/bin/omamobile-aoc-start).
- **Shutdown hangs** end in a watchdog reset after a minute (`RebootWatchdogSec=1min`, [`10-omamobile-reboot-watchdog.conf`](packages/omamobile-device-cheetah/vendor/systemd/system.conf.d/10-omamobile-reboot-watchdog.conf)). A hang inside the kernel's `device_shutdown` isn't covered: an oops that leaves a device lock held (as unloading the AoC modules once did) needs a forced restart.
- **When the phone hangs:**
  1. Wait about 2 minutes. A dead-man undo, a watchdog reset or `panic=10` may bring it back on its own.
  2. Unplug and replug the cable once.
  3. Force a restart: hold Power for about 30 s until the phone is off, then press Power.
  4. Reach fastboot by hand: with the phone off, hold Volume Down, add Power, and keep both held about 20 s until "Fastboot Mode" appears. Letting go early lands on the unlocked-bootloader warning, which carries on booting slot B.
  5. From fastboot, flash known-good images to slot B, or `fastboot --set-active=a` to boot stock Android.
- **Slot fallback:** a slot B that hasn't been marked successful falls back to stock slot A after the bootloader's retries run out. Once `omamobile-boot-success` has marked slot B, don't rely on that fallback: use fastboot. Keep a known-good image set (we keep one in `~/android/mainline/known-good`).

### 3.5 Never touch PMU registers from userspace; mind `devmem`

- **PMU registers are secure-only.** Plain `/dev/mem` writes are silently ignored, and a single `/dev/mem` **read** of a PMU G3D register crashed the phone ([GPU-PLAN.md](device/cheetah/GPU-PLAN.md)). That includes `debugfs/regmap/*pmu*/registers`, which reads the same range raw. Go through the kernel's `exynos-pmu` regmap, which uses an SMC.
- **Read only registers you know are powered and clocked.** On an unpowered block a read aborts the SoC (SError): the AoC PCU while the AoC is in reset, the PCIe ELBI while the PHY is isolated, some HSI2 SysMMU groups. On a first probe, read one register per call.
- **Our `devmem` is `devmem ADDR [VALUE]`.** The second argument is a value to write, not a width: `devmem ADDR 32` **writes 0x20** to ADDR. This has bitten us once already, on a live display register.

### 3.6 Staged bring-up: read-only first, one change per boot

- **Plans run in gates.** Read-only probes come first, then a single write, then a driver probe, then real use. Every step has an expected result, a stop condition and a rollback ([§5.5](#55-use-the-plans-as-templates)).
- **One change per boot.** If two new things break together, you learn nothing.
- **New DT nodes are `status = "disabled"` in the board DTS** and switched on by a bring-up overlay ([`device/cheetah/overlays/`](device/cheetah/overlays)). When a feature has passed every gate, it becomes default-on in a separate commit.
- **Blacklist the module before installing a DTB that enables its node**, so it can't autoload. Load it by hand with `dmesg -w` running.
- **Remove bring-up blacklists** (`/etc/modprobe.d/omamobile-*-bringup.conf`) when the feature moves into the device package. A stale one once left the GPU unbound. The same goes for a package's own modprobe file copied to `/etc/modprobe.d` by hand: a file there replaces the package's `/usr/lib/modprobe.d` file of the same name, so the package's later changes never apply.
- **Don't unload a bring-up module to retry** unless its exit path is known good. On cheetah, `rmmod` of the AoC modules left freed drivers on the platform bus, oopsed the deferred-probe worker and hung every shutdown after it. Reboot instead.

## 4. The development loop

```sh
# Kernel: defconfig + kernel/configs/omamobile.config, built in the container
bin/omamobile kernel           # Image, DTBs, modules into $KERNEL_OUT
bin/omamobile modules          # stripped modules tarball for the Arch root
bin/omamobile initramfs        # Alpine + dropbear + the shared initramfs/init
bin/omamobile images           # boot, init_boot, vendor_boot, vendor_kernel_boot, dtbo

# A first install: packages, root image, then the installer (phone in fastboot)
tools/build-packages           # [omamobile] for aarch64 into out/repo, signed
tools/build-rootfs             # out/omamobile-rootfs-<date>-aarch64.img.zst
tools/install-phone --dry-run  # checks, questions, the exact commands; then without --dry-run
tools/phone-provision FACTORY.zip   # after the first boot: firmware and per-phone data

# Phone in fastboot (developer loop, or when Linux won't boot)
bin/omamobile fastboot         # get there without buttons, if possible
bin/omamobile flash            # flash the five *_b images, set slot B active, boot

# Phone running our Linux (normal loop: no fastboot)
bin/omamobile push             # modules-push if needed, write slot B over SSH, verify, reboot
bin/omamobile modules-push     # modules only, no reboot
tools/phone-ssh <command>      # root@172.16.42.1 over USB
tools/phone-mirror             # the phone's screen in a window here, mouse and keyboard included (omamobile-mirror)
```

- **The device** comes from `DEVICE` (default `cheetah`, in [`omamobile.conf`](omamobile.conf)) and its descriptor `device/<codename>/device.conf`: `DEVICE=<codename> bin/omamobile images`. How builds and releases work for several devices: [docs/RELEASES.md](docs/RELEASES.md).
- **One worktree per subsystem branch.** Each branch gets its own output directory (`KERNEL_DIR=~/android/mainline/wt-gnss KERNEL_OUT=~/android/mainline/out-gnss bin/omamobile kernel`), so builds don't step on each other.
- **The kernel command line goes in the DTB** (`/chosen/bootargs`), not in `vendor_boot`: the Pixel bootloader (ABL) ignores the vendor_boot cmdline.
- **Building a DTB with a bring-up overlay** (the helper the runbooks use):

  ```sh
  O=$KERNEL_OUT D=$O/arch/arm64/boot/dts/exynos/google/gs201-cheetah.dtb
  ov() {  # ov NAME...: rebuild the DTB from default + overlays, then images
    cp -n "$D" "$D.default"; local o= n
    for n; do $O/scripts/dtc/dtc -q -I dts -O dtb -o out/$n.dtbo device/cheetah/overlays/$n.dtso; o="$o out/$n.dtbo"; done
    $O/scripts/dtc/fdtoverlay -i "$D.default" -o "$D" $o
    bin/omamobile images
  }
  # rollback: cp "$D.default" "$D"; bin/omamobile images && bin/omamobile push
  ```

  Don't rely on the applied node order: where a driver numbers things by node order (an ASoC card numbers its PCMs by DAI link order), make the driver order them explicitly.

- **Packages** build natively on the phone: `tools/phone-install-package packages/<name>` builds one with makepkg and installs it with pacman. The maintainer publishes the signed `[omamobile]` repository with `tools/build-packages` (built on the laptop, in an Arch Linux ARM container under qemu-user; no phone needed, the default) and `tools/repo-push`. `tools/repo-build` builds on a connected phone instead, an opt-in shortcut for the packages that take hours under qemu (Mesa, Hyprland, libcamera).
- **Shell UI** can be developed on the desktop in a nested Hyprland: `phone/dev/run-desktop` ([phone/README.md](phone/README.md)).
- **Getting Arch onto `userdata`** (an ext4 Arch Linux ARM root with Omarchy's packages, which the initramfs hands over to): `tools/build-packages` fills `out/repo`, `tools/build-rootfs` builds the root image from it, and `tools/install-phone` (phone in fastboot) asks for the user name, password, Wi-Fi and SSH key, writes them into a local copy of the image, and flashes only slot B's boot images and `userdata`. On the first boot the initramfs grows the image to the whole partition and `omamobile-firstboot` creates the user and runs Omarchy's setup; then `tools/phone-provision FACTORY.zip` puts back Google's firmware (from your own factory image) and the phone's own data. `--dry-run` shows every command first. Flashing `userdata` erases stock Android's data: slot A will need a factory reset if you ever boot it again. The full gated procedure: [REINSTALL-RUNBOOK](device/cheetah/REINSTALL-RUNBOOK.md); the design: [docs/RELEASES.md](docs/RELEASES.md) §6.

## 5. Bringing up a new device

The order below is the one that worked for cheetah. Expect each step to take longer than you think.

### 5.1 Capture the stock facts

On stock Android, before you change anything:

```sh
bin/omamobile capture <codename>-stock          # over adb; or in fastboot: getvar all
```

- **Without root** this saves `getprop`, the partition list (`/dev/block/by-name`), cpuinfo, meminfo, the kernel command line and the vendor module list.
- **With root** it also saves `/sys/firmware/fdt` (the **live device tree**, which is what the bootloader actually handed the kernel, overlays included), `dmesg`, `/proc/iomem`, `/proc/interrupts`, `/proc/modules` and the vendor firmware list. Get root from a userdebug LineageOS build (`bin/lineage-ref`) or your usual root method.
- Decompile the device tree with `dtc -I dtb -O dts live.dtb > live.dts`.
- Run `fastboot getvar all` in fastboot as well: it gives partition sizes, slot state and bootloader versions.
- The cheetah captures behind every plan were made exactly this way. The plans cite them as `R/live.dts:<line>`, `R/iomem.txt` and so on.
- **Captures contain serial numbers, the IMEI and MAC addresses. Keep them in `~/android/reference/`, never in the repository.** When you need a fact from one in a commit or a plan, quote the fact, not the file.

### 5.2 Find the closest supported SoC

| Your device | SoC | Start from |
| --- | --- | --- |
| Pixel 7, 7a, Fold, Tablet | gs201 (Tensor G2) | our `gs201.dtsi` and every gs201 driver; mostly a board DTS |
| Pixel 6, 6 Pro, 6a | gs101 (Tensor) | upstream mainline `gs101.dtsi`, `gs101-oriole.dts` and `gs101-raven.dts`; much of our gs201 work was ported *from* gs101 |
| Pixel 8, 8 Pro, 8a | zuma (Tensor G3) | not started: new clock, pinctrl and SoC data; use gs201 as the pattern |
| Pixel 9 family | zumapro (Tensor G4) | not started |
| Pixel 10 family | laguna (Tensor G5) | not started; a new SoC vendor generation, expect more differences |

The same SoC does **not** mean the same board. The panel, touch controller, PMIC rails, GPIO assignments, battery model and firmware names all differ between phones. **Never copy interrupt numbers between SoCs**: gs201's GIC SPIs differ from gs101's, and gs201's EINT numbering even skips EINT19.

### 5.3 Write the board DTS

- The board DTS lives in the **kernel repository** ([linux-omarchy-mobile](https://github.com/derekross/linux-omarchy-mobile)), not here. Start from the SoC `.dtsi` (`arch/arm64/boot/dts/exynos/google/gs201.dtsi` or `gs101.dtsi`) and from the live stock DT. Add `<soc>-<codename>.dts` with `compatible = "google,<soc>-<codename>", "google,<soc>"`, plus its entry in `Documentation/devicetree/bindings/arm/google.yaml`.
- Read [BRINGUP-M1.md](device/cheetah/BRINGUP-M1.md) §1 (the boot chain) and §4 (the minimal DTS) first. The Pixel bootloader has rules of its own:
  - It picks the base DTB by `/soc_id`. Ship one copy per silicon revision.
  - It requires `/chosen/bootargs` and an empty `/ect` node.
  - It applies a board `dtbo` keyed by `board_id`/`board_rev`. Ship a trivial one with your device's IDs (from `getprop ro.boot.dtbo_idx` and the stock `dtbo.img`).
  - It applies a debug-policy overlay from the `dpm` partition, so build with `-@` and add the label stubs it targets.
  - It injects `/memory` and the framebuffer carve-out itself.
  - It leaves the cluster watchdogs running, which resets the phone after about 60 s unless a watchdog driver takes them over.
- **Check every stock `reserved-memory` node against `/proc/iomem`.** Firmware that writes into RAM the kernel thinks is free corrupts data silently. Mind the cell sizes: on gs201 each `reg` entry is 12 bytes, and a wrong length makes the kernel ignore the property without a word.
- **Write the device descriptor**, `device/<codename>/device.conf`, starting from [cheetah's](device/cheetah/device.conf): the DTB name, the `soc_id` values (from the live DT and `fastboot getvar all`), the board dtbo source and its `board_id`/`board_rev`, the boot images the phone has (`fastboot getvar all` lists the partitions; older Pixels lack `init_boot` and `vendor_kernel_boot`), the storage-guard allowlist and the panel size. Then every `bin/omamobile` command takes the device from the environment: `DEVICE=<codename> bin/omamobile images`. [docs/RELEASES.md](docs/RELEASES.md) §3 describes each key and what `bin/omamobile` checks. The allowlist may only name `*_b` partitions and `userdata`.
- The initramfs `init` is shared ([`initramfs/init`](initramfs/init)). Do not copy it. Set `INITRAMFS_INIT=/initramfs/init`. The display trigger is `SIMPLEFB_TRIG_CON="0xADDR 0xVALUE"` in the descriptor. `bin/omamobile initramfs` writes that pair into the image, and the init runs it only when the board's first compatible string matches the descriptor's `COMPATIBLE`. Leave the key unset and the init writes no display register. **Never put another phone's address in the descriptor.** `probe()` runs only when `BRINGUP_PROBE=1` and the board's first compatible string matches `COMPATIBLE`. Those reads cover every display DMA channel, so leave the key unset until this phone's panels are driven. `UFS_RW` in the shared init must match the descriptor. The build checks. The rest of the per-device init (the USB product string, the main LUN) is still step 2 of the release track ([ROADMAP](docs/ROADMAP.md) §3.8).

### 5.4 Bring-up order that worked

Each stage has a plan document for cheetah. Use it as the map and the template for your device.

| # | Stage | What "works" means | cheetah plan |
| --- | --- | --- | --- |
| 1 | **First light**: kernel boots from slot B, console on the bootloader's framebuffer (`simple-framebuffer`), USB gadget networking, SSH, watchdogs handled, `panic=10` | `ssh root@172.16.42.1` | [BRINGUP-M1.md](device/cheetah/BRINGUP-M1.md) |
| 2 | **Clocks, pinctrl, storage**: the SoC clock driver, then UFS with the read-only guard, then the Arch root on `userdata` | Arch boots from UFS | [BRINGUP-M1.md](device/cheetah/BRINGUP-M1.md), [NOTES.md](device/cheetah/NOTES.md) |
| 3 | **Touch and buttons** (SPI/I2C touch controller, `gpio-keys`) | Hyprland sees touch, `evtest` sees keys | [NOTES.md](device/cheetah/NOTES.md) |
| 4 | **GPU**: Panthor plus the Mesa model-table patch ([`packages/mesa`](packages/mesa)) | Hyprland renders on the GPU | [GPU-PLAN.md](device/cheetah/GPU-PLAN.md) |
| 5 | **Display**: simpledrm, then a DSI command path (brightness), then a flip-only KMS driver and the panel's high refresh rate | brightness slider, 120 Hz, DPMS | [DISPLAY-PLAN.md](device/cheetah/DISPLAY-PLAN.md) |
| 6 | **I2C, fuel gauge, battery model** | correct capacity and temperature | [WIFI-BATTERY-PLAN.md](device/cheetah/WIFI-BATTERY-PLAN.md) B1 |
| 7 | **Wi-Fi** (PCIe controller, brcmfmac, DMA confinement) | connects, no hangs | [WIFI-BATTERY-PLAN.md](device/cheetah/WIFI-BATTERY-PLAN.md) W1-W4 |
| 8 | **Bluetooth** (UART, hci_bcm) | pairing, A2DP audio | [BLUETOOTH-PLAN.md](device/cheetah/BLUETOOTH-PLAN.md) |
| 9 | **Haptics and torch** | feedbackd buzzes, torch tile | [HAPTICS-TORCH-PLAN.md](device/cheetah/HAPTICS-TORCH-PLAN.md) |
| 10 | **ACPM** (the firmware power manager): GPU DVFS, PMIC, thermal | GPU scales | [ACPM-PLAN.md](device/cheetah/ACPM-PLAN.md) |
| 11 | **CPU idle, UFS power** | measured drain drops | [POWER-PLAN.md](device/cheetah/POWER-PLAN.md) |
| 12 | **USB-C and charging** (TCPC, role switch, charger, temperature guard) | replug works, charges safely | [USB-C-PLAN.md](device/cheetah/USB-C-PLAN.md) |
| 13 | **Suspend** (s2idle, then deep sleep) | sleeps and wakes on Power | [SUSPEND-PLAN.md](device/cheetah/SUSPEND-PLAN.md) |
| 14 | **AoC** (audio and sensor DSP, started through the GSA and Trusty), then sensors, then speakers and mics | services online; auto-rotate; sound | [AOC-PLAN.md](device/cheetah/AOC-PLAN.md), [USF-PROTOCOL.md](device/cheetah/USF-PROTOCOL.md), [AUDIO-PLAN.md](device/cheetah/AUDIO-PLAN.md) |
| 15 | **GNSS, NFC, modem, cameras** | a fix; calls and data later | [RADIOS-CAMERA-PLAN.md](device/cheetah/RADIOS-CAMERA-PLAN.md), [GNSS-PLAN.md](device/cheetah/GNSS-PLAN.md), [MODEM-PLAN.md](device/cheetah/MODEM-PLAN.md) |
| 16 | **Fingerprint** (Trusty, the RPMB, Google's HAL in a chroot) | unlocks Omarchy's lock | [FINGERPRINT-PLAN.md](device/cheetah/FINGERPRINT-PLAN.md), [FINGERPRINT-RESEARCH.md](device/cheetah/FINGERPRINT-RESEARCH.md) |

Stages 1-5 make a usable phone on a desk. After stage 2, most stages are independent of each other, so pick what you care about.

### 5.5 Use the plans as templates

Write `device/<codename>/<SUBSYSTEM>-PLAN.md` before you write code. Every cheetah plan has the same shape:

1. **Status line** (dated): which branch and worktree, what was built, what has run on the phone.
2. **Goal**, in order of value.
3. **Path abbreviations** for citations (`G/` = Google's GPL kernel, `R/` = your stock capture, `L/` = our kernel).
4. **Facts with evidence.** Each is marked [V] (checked, with `file:line`) or [I] (an inference to check on the device).
5. **Design and options**, with the reason for the choice.
6. **What was built**: a table of kernel commits and repo changes.
7. **Risks**, and what would hurt the phone.
8. **A gated runbook.** Each step lists the commands, **Expect/Pass**, a **stop condition**, a **Rollback** and a risk level. Read-only steps come first.
9. **Results**, appended with dates and the kernel release that produced them, plus **open questions**.

Findings that matter beyond one subsystem go into `device/<codename>/NOTES.md`, newest first.

## 6. Submitting changes

### 6.1 Where things go

| Change | Repository | Branch or path |
| --- | --- | --- |
| Kernel: drivers, bindings, DTS | [derekross/linux-omarchy-mobile](https://github.com/derekross/linux-omarchy-mobile) | fork it, branch off `omamobile`, one subsystem per branch (`omamobile-<subsystem>`, e.g. `omamobile-gs101-oriole-dts`), and open a pull request against `omamobile` |
| Device notes, plans, descriptor, bring-up overlays | this repository | `device/<codename>/` (descriptor `device.conf`), overlays in `device/<codename>/overlays/` |
| Device support package | this repository | `packages/omamobile-device-<codename>/` |
| Shell, sensors, GNSS, Mesa, Hyprland | this repository | `packages/<name>/` |
| Omarchy changes | upstream [Omarchy](https://github.com/omacom/omarchy) first | `patches/omarchy/` while they wait |
| Tools | this repository | `tools/`, `bin/` |

**Device tree nodes go in the kernel; overlays only switch them on.** A new block (a sensor, a regulator, a bus) is described in the board DTS in linux-omarchy-mobile with `status = "disabled"`, next to its binding and driver. The `.dtso` files in this repository's `device/<codename>/overlays/` only set nodes to `"okay"` (and add bring-up-only tweaks) for one gated test boot; once a node is proven, it is enabled in the DTS itself and its overlay goes away. A driver, binding or DTS change never belongs in this repository, and a bring-up overlay never belongs in the kernel.

### 6.2 Kernel patches, upstream-style

- **One subsystem per branch.** Inside it, one logical change per commit, in upstream order: `dt-bindings:` first, then the driver, then `arm64: dts:`. Each commit must build.
- **Prefixes** follow `git log --oneline <file>` for the files you touch, for example `clk: samsung: gs201: …`, `arm64: dts: exynos: google: gs201-cheetah: …`, `dt-bindings: soc: google: …`.
- **Messages explain why**, with evidence: the stock DT line, Google's GPL source, and what was measured on the phone. Use the imperative mood and wrap at 72 columns.
- **Checks before you open a PR:**
  - `scripts/checkpatch.pl --strict` on each patch;
  - a build with `W=1` and no new warnings;
  - `make dt_binding_check DT_SCHEMA_FILES=<your schemas>`;
  - `make CHECK_DTBS=y W=1 exynos/google/<soc>-<codename>.dtb` (or `dt-validate`), with no new warnings, also with your bring-up overlays applied.

  The plans record these results (for example [HAPTICS-TORCH-PLAN.md](device/cheetah/HAPTICS-TORCH-PLAN.md)).
- **New hardware is `disabled` by default.** A separate, later commit turns it on once it has passed on the phone.
- **Code imported from Google's GPL tree** keeps its copyright and is marked as such. Port it to upstream APIs and style before proposing it for `omamobile`.
- **Sending upstream:** the published `omamobile` branch sits on one squashed commit that holds the tree of the upstream tag (see [`tools/publish-kernel`](tools/publish-kernel)). Rebase your commits onto the real tag (`git rebase --onto v7.3-rc6 <squashed base>`), run `scripts/get_maintainer.pl`, and follow `Documentation/process/submitting-patches.rst`.
- **Sign-off and AI tools.** Upstream needs your own `Signed-off-by:` (the DCO). If an AI assistant helped, follow `Documentation/process/coding-assistants.rst`: the human signs off, the AI never does, and its help is credited with an `Assisted-by:` tag.

### 6.3 This repository

- **Commit subjects** name the area, then say what changed: `omamobile-gnss: package GNSS with location on demand`, `device-cheetah 0.1.15: reconnect USB after a replug`, `shell 0.1.17: …`, `DISPLAY-PLAN: D3b-1b result, …`, `NOTES: …`. When a commit changes a package, bump its `version` file and put the new version in the subject.
- **Bodies** explain the why and what was tested. Wrap at 72 columns. If an AI assistant co-wrote the commit, end with a `Co-Authored-By:` trailer, as the history does.
- **Every file has an SPDX header** (§7).
- **Packages** follow the existing layout: `PKGBUILD`, `install`, `version`, `README.md`, and `test/all`, which must pass. Device packages are named `omamobile-device-<codename>`. Nothing in them is enabled unless it's safe at boot, with hang markers where needed.
- **Keep identifying data out.** No Wi-Fi names, MAC or Bluetooth addresses, serial numbers, IMEIs, account names or captures: use placeholders such as `<SSID>`. Screenshots must not show any of these either.
- **Open a PR** with what you ran on the phone, the kernel release, and the plan section it follows. Kernel PRs go to your fork's topic branch against linux-omarchy-mobile's `omamobile`; everything else to this repository's `main`. A new device's first PR here is usually its `device/<codename>/device.conf` and notes (§5).

### 6.4 Adding your device to the matrix

Add a row to [docs/DEVICES.md](docs/DEVICES.md) in the same PR as your device notes. Only claim "works" for what you've verified on your own phone, and link the notes entry that shows it.

## 7. Licensing and vendor firmware

- **This repository is MIT** ([LICENSE](LICENSE)), except where a file's SPDX header says otherwise. That covers the tools, packages, docs, bring-up overlays, the initramfs and the fingerprint and GNSS chroot pieces. Packages that are installed on their own also carry their own MIT `LICENSE`. Every file of ours starts with an SPDX header; new files get `SPDX-License-Identifier: MIT`.
- **Kernel patches are GPL-2.0** (`GPL-2.0-only`, or whatever the subsystem uses), and so are the DTS files that go into the kernel. In this repository, the board `dtbo` source ([`cheetah-m1-overlay.dts`](device/cheetah/cheetah-m1-overlay.dts)) is GPL-2.0-only for the same reason.
- **Exceptions keep their upstream licence:**
  - the port of Google's Apache-2.0 boot-control code keeps that licence and Google's notice ([`omamobile-bootctl.c`](packages/omamobile-device-cheetah/src/omamobile-bootctl.c), with `LICENSE.Apache-2.0` beside it);
  - PKGBUILDs derived from Arch's are 0BSD;
  - patches to Mesa, Hyprland and Omarchy are under those projects' licences.
- **Only use code from properly licensed sources.** Google's GPL kernel, upstream Linux, postmarketOS and Mesa are fine. Leaked or unlicensed code is not, and neither is decompiled proprietary code: describe protocols (as [USF-PROTOCOL.md](device/cheetah/USF-PROTOCOL.md) does), don't copy code.
- **Google's proprietary firmware and binaries are never committed, attached or redistributed.** That covers Wi-Fi, Bluetooth, haptics, AoC and GNSS firmware, vendor HALs and libraries, calibration and tuning files. Our scripts take them from **the user's own factory image** on the user's own machine:
  - [`tools/extract-vendor-firmware`](tools/extract-vendor-firmware): firmware, renamed to what mainline drivers request;
  - [`tools/extract-vendor-chroot`](tools/extract-vendor-chroot) and [`tools/extract-fingerprint-vendor`](tools/extract-fingerprint-vendor): the minimal Android trees for the GNSS and fingerprint chroots, with a `MANIFEST` of checksums.

  A new device adds its own mapping to these scripts, not the files.
