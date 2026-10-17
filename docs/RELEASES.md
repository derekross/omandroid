<!-- SPDX-License-Identifier: MIT -->
# Builds and releases for many devices

How Omarchy Mobile goes from one hand-built Pixel 7 Pro to a project that builds and releases for several Pixels with the same tools. This is the design; the order of the work is the "Multi-device builds and releases" track in [ROADMAP.md](ROADMAP.md) §3.8. Status (2026-10-13): step 1 (the device descriptor and `DEVICE=` in `bin/omamobile`) is done for cheetah, and so is most of §6: the root image (`tools/build-rootfs`), the installer (`tools/install-phone`, the bundle's `install`), the first-boot setup (`omamobile-firstboot`), the bundle (`tools/make-bundle`), aarch64 package builds without a phone (`tools/build-packages`) and per-phone provisioning (`tools/phone-provision`). They are tested offline (`tests/all`) and not yet run on a phone: [REINSTALL-RUNBOOK](../device/cheetah/REINSTALL-RUNBOOK.md) is their first use. The kernel package (§4), the package hook (§5) and signed releases (§7-§11) are still design.

The safety model in [CONTRIBUTING.md](../CONTRIBUTING.md) §3 and [AGENTS.md](../AGENTS.md) §1 is not up for redesign here. Every tool, hook and installer below keeps it:

- Omarchy Mobile lives on **slot B and `userdata` only**. Slot A keeps stock Android as the way back.
- Nothing ever writes `bootloader`, `abl`, `radio`/`modem`, `devinfo` (apart from the existing boot-success marker), `persist`, `efs*`, `modem_userdata` or `vbmeta`. Nothing runs an unsuffixed `fastboot flash`, `fastboot -w`, `--slot=all` or `flashing lock`.
- The initramfs storage guard runs before anything else, and its allowlist works by GPT name.
- Google's firmware and binaries are extracted on the user's own machine from the user's own factory image, and never redistributed: not in packages, not in bundles, not in CI artifacts.
- Signing keys never enter a repository or a CI system.

## 1. The model

- **One kernel for every device.** One arm64 `Image` and one set of modules, built from `linux-omarchy-mobile`, with every board's DTB side by side in the kernel tree: `gs201-cheetah.dts` today, `gs201-panther.dts`, `gs201-lynx.dts`, `gs101-oriole.dts`, `gs101-raven.dts` later. A board needs its own DTB, not its own kernel.
- **Per device, five things:**
  1. the board DTB (in the kernel);
  2. bootloader glue: the `soc_id` copies of the DTB, the board `dtbo` with its `board_id`/`board_rev`, the boot image set and header values;
  3. initramfs quirks: the storage-guard allowlist, the display trigger write, the USB gadget's name;
  4. firmware, extracted from the user's factory image by a per-device map;
  5. `omamobile-device-<codename>`, the userspace support package.
- **One descriptor per device**, `device/<codename>/device.conf`, holds the values of 2 and 3 and points at 4 and 5, so no tool carries a codename in its code.
- **Releases** ship through the signed `[omamobile]` pacman repository: a `linux-omamobile` kernel package (`Image`, modules, all DTBs), device packages that pick their DTB and write slot B through a pacman hook, and, for first installs, per-device flashable bundles.

## 2. What is cheetah-specific today

The audit behind this design (2026-10-13). "Step" names the roadmap step that removes it.

| Where | What is hard-coded | Step |
| --- | --- | --- |
| `bin/omamobile images` | DTB path `gs201-cheetah.dtb`; `soc_id` 0x9855001 and the 0x9855000 rewrite; the `cheetah-m1-overlay.dts` dtbo and `--id=0x30306 --rev=0x10000`; the vendor cmdline (`earlycon` at gs201's UART); `--os_version`/`--os_patch_level`; the vendor page size | 1 (done) |
| `bin/omamobile initramfs` | the splash size comes from `PANEL_SIZE`; C sources from the device package when it has them. The init path and the trigger write come from the descriptor | 1 (done) |
| `bin/omamobile capture` | the default capture name `cheetah-<date>` | 1 (done) |
| `bin/omamobile push`, `flash` | the partition lookup assumes the main LUN is `sda` | 1 (allowlist check and `BOOT_IMAGES` layouts done; the Pixel 6 has no `init_boot` or `vendor_kernel_boot`, [oriole NOTES](../device/oriole/NOTES.md)), 3 |
| `initramfs/init` | `UFS_RW` (checked against the descriptor at build time); `probe()` when `BRINGUP_PROBE=1` and the board matches `COMPATIBLE` (watchdog and DPP reads); the USB product string "Pixel 7 Pro (M1)"; the main LUN `sda`. The TRIG_CON write is `SIMPLEFB_TRIG_CON` in the descriptor, packed into that device's initramfs only | 2 (trigger done; the rest still step 2) |
| `bin/omamobile initramfs` | the developer's `~/.ssh/*.pub` baked in as `authorized_keys` (fine for development, never for a release) | 2 |
| `tools/extract-vendor-firmware` | `BOARD="google,gs201-cheetah"`, `DEVICE="cheetah"`, the firmware `MAP` | 4 |
| `tools/extract-vendor-chroot`, `extract-fingerprint-vendor` | `DEVICE="cheetah"`, cheetah's file lists | 4 |
| `tools/aoc-stage` | the AoC firmware path under `vendor/google/cheetah` | bring-up tool, per plan |
| `tools/display-hz` | DECON register addresses (gs201) | bring-up tool, per plan |
| `tools/repo-build` | the default package list names `omamobile-device-cheetah` | 3 |
| `CONTRIBUTING.md` §4 `ov` helper | `gs201-cheetah.dtb`, `device/cheetah/overlays` | 2 (`bin/omamobile overlay`) |
| `packages/omamobile-modem` | `depends=(… omamobile-device-cheetah)` | 3 (depend on a virtual `omamobile-device`) |
| `packages/omamobile-gnss`, `-modem` | `ro.hardware=cheetah` in the chroot props | per device, with its chroot map |
| `packages/omamobile-audio` | the UCM profile `Google/cheetah/HiFi.conf`, the speaker stages | per device (audio is board tuning) |
| `packages/omamobile-device-cheetah` | `gs201_aoc_rails`, `pcie_gs201`, modprobe and udev policy | stays per device: this is what the package is for |

Bring-up tools (`*-stage`, `display-hz`, `usbc-deadman`) stay per plan: they poke one SoC's registers on purpose, and a second device gets its own copy only after its own read-only probes.

## 3. The device descriptor

`device/<codename>/device.conf`, one per device. [cheetah's](../device/cheetah/device.conf) is the reference.

**Format.** `KEY=value` or `KEY="value"` lines, comments and blank lines. No `$`, backticks, backslashes or single quotes, so it is data, not shell: `bin/omamobile` parses it line by line (it never sources it) and refuses anything else, and Python tools, the pacman hook and the initramfs can read it the same way. Lists are space-separated inside quotes.

### 3.1 Keys in use (step 1)

| Key | cheetah | Meaning |
| --- | --- | --- |
| `CODENAME` | `cheetah` | Must equal the directory name and `DEVICE` |
| `NAME` | `Pixel 7 Pro` | Marketing name, for messages and bundle READMEs |
| `SOC` | `gs201` | SoC family |
| `COMPATIBLE` | `google,gs201-cheetah` | First DT root compatible; firmware names and the device check at install time use it |
| `PACKAGE` | `omamobile-device-cheetah` | The device's package (and, for now, where the initramfs C helpers' sources live) |
| `DTB` | `exynos/google/gs201-cheetah.dtb` | Under `arch/arm64/boot/dts` in the kernel build, and under `dtbs/` in the kernel package |
| `SOC_IDS` | `b0:0x9855001 a0:0x9855000` | `label:soc_id`. The first is what the DTB is built with; each other entry is a copy with `/soc_id` rewritten. Order is the order in `dtb.img` |
| `DTBO_SOURCE` | `cheetah-m1-overlay.dts` | The board dtbo source beside the descriptor (GPL-2.0) |
| `DTBO_BOARD_ID`, `DTBO_BOARD_REV` | `0x30306`, `0x10000` | What ABL matches the dtbo entry on; checked against the source |
| `VENDOR_CMDLINE` | `console=tty0 earlycon=exynos4210,0x10A00000 …` | `vendor_boot` command line (kernel arguments really go in `/chosen/bootargs`) |
| `VENDOR_PAGESIZE` | `0x800` | Page size of `vendor_boot` (and `vendor_kernel_boot`, where the device has one) |
| `OS_VERSION`, `OS_PATCH_LEVEL` | `17.0.0`, `2026-07` | The `init_boot` header values (the `boot` header's, on a device without `init_boot`) |
| `BOOT_IMAGES` | `boot init_boot vendor_boot vendor_kernel_boot dtbo` | The images written to slot B (the `_b` suffix is added by the tools and is not configurable). `boot`, `vendor_boot` and `dtbo` are always there. Without `init_boot`, `boot` carries the empty generic ramdisk and the OS version; without `vendor_kernel_boot`, `vendor_boot` carries the DTB and the initramfs (the Pixel 6: `boot vendor_boot dtbo`) |
| `UFS_RW` | `boot_b … dtbo_b userdata` | The storage guard's allowlist. Only `*_b` names and `userdata` are accepted, and every `BOOT_IMAGES` entry must be on it |
| `INITRAMFS_INIT` | `/initramfs/init` | The init script. A leading `/` is relative to the repo root, so devices share one script. Any other value is relative to the descriptor |
| `SIMPLEFB_TRIG_CON` | `0x1c240030 0x3061` | This device's simplefb trigger write, `addr value`, packed into its initramfs next to `COMPATIBLE`. The init writes it only when that string is the board's first compatible. Absent or empty: no display write. Felix: `0x1c241030 0x3061` (cover, DECON1) |
| `BRINGUP_PROBE` | `1` | Install the boot probe that reads watchdog and DPP registers. The init runs it only when the board matches `COMPATIBLE`. Absent on a phone whose panels are not driven yet |
| `PANEL_SIZE` | `1440x3120` | Portrait panel size for the early splash |

### 3.2 Keys to add later

| Key | Step | Meaning |
| --- | --- | --- |
| `USB_PRODUCT` | 2 | The USB gadget product string. The shared init still says "Pixel 7 Pro (M1)" |
| `MAIN_LUN` | 2 | The UFS LUN holding the partition table with slot B and `userdata` (`sda`) |
| `FIRMWARE_MAP` | 4 | A file beside the descriptor (`firmware.map`: `source-in-vendor|destination` lines), replacing the arrays in `tools/extract-vendor-firmware` |
| `FACTORY_PRODUCT` | 4 | The `board=` value in the factory image's `android-info.txt`, and `fastboot getvar product`, checked before extraction and before flashing |
| `USERDATA_FS` | 4 | `ext4`; the installer grows the root to fill `userdata` on first boot |

### 3.3 Selecting a device

`DEVICE` picks the descriptor. `omamobile.conf` sets `DEVICE="${DEVICE:-cheetah}"`, so the environment wins, then `omamobile.local.conf`:

```sh
bin/omamobile images                    # cheetah
DEVICE=panther bin/omamobile images     # device/panther/device.conf
```

`DEVICE` must be a bare codename. `bin/lineage-ref` reads the same variable for its LineageOS target. Commands that don't depend on a device (`kernel`, `modules`, `modules-push`, `kexec`, `fastboot`, `kernel-shell`) don't read the descriptor. `capture` doesn't need one either, since a new device has no descriptor until after its captures.

What `bin/omamobile` checks, so a wrong descriptor fails the build rather than the phone:

- the descriptor's format, required keys and `CODENAME`;
- `UFS_RW` holds only `*_b` and `userdata`, and covers every `BOOT_IMAGES` entry;
- `BOOT_IMAGES` names only `boot`, `init_boot`, `vendor_boot`, `vendor_kernel_boot` and `dtbo`, each once, and includes `boot`, `vendor_boot` and `dtbo`;
- `UFS_RW` in the initramfs `init` is the descriptor's, byte for byte (until step 2 generates it);
- the DTB really is built with the first `SOC_IDS` value, and each rewrite took;
- the dtbo source carries `DTBO_BOARD_ID` and `DTBO_BOARD_REV`;
- `push` and `flash` refuse any image whose `_b` partition is not on the allowlist.
- `images` records the device in `images/DEVICE`, and `push`, `flash` and `tools/make-bundle` refuse images built for another device.

Step 1 changed no output for cheetah: `images` produced the same checksums for every file before and after, and `initramfs` staged an identical tree (§9).

## 4. The kernel package: `linux-omamobile`

One package for every device, in `[omamobile]`.

- **Source:** a tagged snapshot of `linux-omarchy-mobile`'s `omamobile-int` branch (§8), built with `kernel/configs/omamobile.config` on arm64 `defconfig`.
- **Contents**, following Arch Linux ARM's layout:
  - `/usr/lib/modules/<release>/vmlinuz`: the uncompressed arm64 `Image` (the hook compresses it with lz4 for `boot.img`);
  - `/usr/lib/modules/<release>/kernel/…`, `modules.*`: stripped modules, as `bin/omamobile modules` packs them today;
  - `/usr/lib/modules/<release>/dtbs/exynos/google/*.dtb`: every Google board DTB the tree builds;
  - `/usr/lib/modules/<release>/pkgbase`: `linux-omamobile`, as Arch kernels do.
- **No initramfs, no boot images.** Those are per device and come from the device package's hook (§5).
- **Release string:** `CONFIG_LOCALVERSION` becomes `-omamobile` for release builds and `CONFIG_LOCALVERSION_AUTO` stays on, so `uname -r` is `7.3.0-rc6-omamobile-00453-g<sha>` and names the snapshot exactly. The `-push1` suffix stays for development builds, so a development kernel can never be mistaken for a packaged one.
- **pkgver:** the release with `-` folded into `.` (pacman forbids `-`): `7.3.0rc6.r453.g<sha>`, which sorts upward as the snapshot grows. `pkgrel` counts packaging-only changes.
- **Build:** cross-compiled in the existing docker environment on x86 (`bin/omamobile kernel` and `modules`, unchanged), then packed by a `PKGBUILD` that only copies the build output (`arch=(aarch64)`, no compile step), so it can run on the laptop under `makepkg --config` with `CARCH=aarch64`. Building the kernel on the phone is possible but slow; the decision is §11.
- **Headers** (`linux-omamobile-headers`) can follow when something needs out-of-tree modules (DKMS); nothing does today.
- **Old modules:** a pacman upgrade removes the previous release's modules. The hook therefore keeps one copy of the previous `/usr/lib/modules/<release>` in `/var/lib/omamobile/boot/previous/` together with the previous slot-B images, so a rollback restores both (§5.3).

## 5. Device package hook

The device package, not the kernel package, turns the kernel into bootable slot-B images, because only it knows the DTB, the `soc_id`s, the dtbo and the initramfs quirks. This is what `bin/omamobile images` plus `push` do today, run on the phone.

### 5.1 What the device package adds

- `/usr/share/omamobile/device.conf`: the descriptor, installed verbatim.
- `/usr/lib/omamobile/<codename>/dtbo.img`: built at package time from `DTBO_SOURCE`.
- `/usr/lib/omamobile/<codename>/initramfs.cpio.lz4`: the release initramfs (§5.2), built at package time.
- `/usr/bin/omamobile-boot-update`: builds the images and writes slot B.
- `/usr/share/libalpm/hooks/90-omamobile-boot.hook`: runs it after a transaction that touches `usr/lib/modules/*/vmlinuz`, `usr/lib/modules/*/dtbs/*`, the descriptor, the dtbo or the initramfs.
- `provides=(omamobile-device)`, `conflicts=(omamobile-device)`: exactly one device package per phone, and packages that need "some device" (`omamobile-modem` today) depend on the virtual name.
- Runtime dependencies: `dtc` (for the `soc_id` copies) and `mkbootimg` (Arch's `android-tools` ships it [I], to check; otherwise the package carries AOSP's Apache-2.0 `mkbootimg.py` with its notice, as the bootctl port does).

### 5.2 The release initramfs

The same Alpine-based initramfs, with release differences:

- **No developer keys.** It ships without `authorized_keys`. The hook appends a second, tiny cpio to the ramdisk that holds the phone's own `/root/.ssh/authorized_keys` (the kernel unpacks concatenated cpio archives in order), so SSH into the initramfs keeps working for the phone's owner and nobody else.
- **No bring-up probes.** `probe()` and `omamobile.debug` stay in development builds only.
- **Quirks from the descriptor.** The trigger write is already per device: `bin/omamobile initramfs` packs `SIMPLEFB_TRIG_CON` into `/etc/omamobile-simplefb-trig` and `COMPATIBLE` beside it. The init writes the pair only when the board's first compatible string matches. A device without the trigger key gets no register write. Step 2 still turns the remaining quirks (`UFS_RW`, `MAIN_LUN`, `USB_PRODUCT`) into a generated `/etc/omamobile/device.conf` the shared init reads at boot. That remaining step changes what boots, so it gets its own runbook and a test boot from `bin/omamobile push`; it is not a refactor.
- **Reproducible:** fixed mtimes (`SOURCE_DATE_EPOCH`), sorted file list, `cpio --reproducible`.

### 5.3 What `omamobile-boot-update` does

1. Reads `/usr/share/omamobile/device.conf` and checks that `/proc/device-tree/compatible` starts with its `COMPATIBLE`. Refuses on a mismatch: a package for one Pixel never writes another's slot B.
2. Finds the newest installed release with a `vmlinuz` and a `modules.dep`, and refuses if `depmod` hasn't run for it (the kernel without its modules boots without its GPU).
3. Builds the images exactly as `bin/omamobile images` does (the same descriptor, the same commands; the shared code moves into one script used by both), into `/var/lib/omamobile/boot/next/`.
4. Skips the write if every image matches what slot B holds now (checksums of the partitions' leading bytes).
5. Saves slot B's current images and the running release's modules to `/var/lib/omamobile/boot/previous/`.
6. Writes each image through the same guarded path as `push`: the partition by `PARTNAME=<image>_b`, `ro` must read 0 (the storage guard decides, the hook never changes it), `dd conv=fsync`, then a read-back checksum. Any mismatch stops the hook with the error at the top of pacman's output and leaves the saved images in place for `omamobile-boot-update --rollback`.
7. **Never reboots.** The shell's update notice says a restart is needed.

**Development phones are opted out.** The hook does nothing unless `/etc/omamobile/boot-update.conf` says `AUTO_WRITE=yes`. Release installs set it; a development phone keeps `bin/omamobile push` as the only writer to slot B, so `omarchy update` never overwrites a test kernel.

**What a bad kernel costs:** slot B has no A/B of its own. If the new images don't boot, ABL's retries run out and the phone falls back to stock Android on slot A (or, once the boot-success marker has marked slot B, needs fastboot). Recovery is the release bundle's installer with the previous bundle, or `fastboot flash <image>_b` with the saved images. That is the same exposure the project has today; making it smaller (a second set of images on spare space, or a boot counter in the initramfs) is a later step.

### 5.4 Updating a phone: `omarchy update` and the Omarchy patches

`omamobile-omarchy` (in every image built from 2026-10-13 on, and a dependency of `omamobile-shell`, so `omarchy update` brings it to older phones) owns both halves; the details are in the headers of its `bin/omamobile-update` and `bin/omamobile-omarchy-patches`.

**The patches survive upgrades.** The series lives in `packages/omamobile-omarchy/patches` (`patches/omarchy` links there) and ships in the package. Its pacman hook (`95-omamobile-omarchy-patches.hook`, PostTransaction on `omarchy`, `omarchy-dev` and itself) re-applies it after every transaction that replaces Omarchy's files, and `rootfs/build-inside` runs the same tool, so image builds and upgrades take one code path. Every run starts from Omarchy's own files (checked against the package's mtree), applies the series in a scratch tree with `--fuzz=0`, verifies it by reversing it back to Omarchy's files, and only then swaps the files in. A patch whose reverse applies is already in Omarchy (a backport that landed): it is skipped and reported, and should be dropped from the series. Any other failure leaves Omarchy's files exactly as shipped, logs to `/var/log/omamobile/omarchy-patches.log` and notifies the user (the lock's PIN pad may be missing until a fixed series ships). A file that matches neither Omarchy's copy nor the last patched one was changed by something else and is never overwritten.

**The update takes the phone's path.** Patch 0012 hands `omarchy update` to `omamobile-update`, which runs Omarchy's own `omarchy-update -y` unchanged, with stand-ins first on `PATH` for the steps that don't belong on a phone. Step by step:

| Omarchy's step | On the phone |
| --- | --- |
| transcript (`script` to `/tmp/omarchy-update.log`) | kept; `omamobile-update` makes the pty itself, so one sudo timestamp covers the whole run |
| update lock, 10 GiB free-space check | kept |
| `omarchy-update-confirm` (gum) | replaced by the sheet's Update button, plus refusals: battery below 30% unless charging; `[omamobile]` not first or `[omarchy]` after Arch Linux ARM's repositories; a kernel, bootloader, Snapper or fwupd package in the transaction |
| `omarchy-update-pkg-prune` (paccache) | kept |
| `omarchy-snapshot create` (Snapper, Limine's snapshot menu) | skipped: ext4 root, no Snapper (exit 127, which Omarchy takes for "Snapper absent") |
| stay-awake inhibitor | kept (the screen stays on during the update) |
| `omarchy-update-dev`, keyrings | kept |
| `pacman -Syu` with Omarchy's guard, `--overwrite /usr/share/omarchy/*` and conflict handler | kept; `[omamobile]` first, so its Mesa, Hyprland and libcamera win; the patch hook runs at the end of the transaction |
| `omarchy-migrate` | kept, but migrations that mention Limine, mkinitcpio, UKIs, efibootmgr, sbctl, Snapper, GRUB, `/boot/`, fwupd, x86 kernels or NVIDIA, or ask with gum, are held: marked done for Omarchy, listed in `~/.local/state/omamobile/update/held-migrations`, and run after a review with `omamobile-update run-migration <name>` |
| `omarchy-hook post-update`, `omarchy-update-mise` | kept |
| `omarchy-update-aur-pkgs` (yay) | skipped: foreign packages are listed, never rebuilt on the phone |
| `omarchy-update-orphan-pkgs` (gum) | orphans listed, never removed |
| `omarchy-update-analyze-logs`, `omarchy-update-status` | kept (the initcpio check finds nothing; the status reaches the update sheet) |
| `omarchy-update-restart` (gum; "kernel updated" whenever no pacman-owned `vmlinuz` matches `uname -r`, which on the phone is always) | replaced: a restart is needed only for Omarchy's `reboot-required` or a replaced Hyprland; marked services and the shell restart as on a desktop; nothing reboots by itself |
| Update > Firmware (fwupd), Update > Config > Plymouth | hidden from the phone's menu (not part of the update) |

The kernel is not touched: it still reaches the phone through `bin/omamobile push` until the boot hook above exists. `OMAMOBILE_UPDATE=1 omarchy-update` runs Omarchy's flow unwrapped (an escape hatch, not for normal use); `pacman -Syu` stays blocked by Omarchy's guard.

**The phone's update UI** is `omamobile-shell`'s `omamobile.update` sheet: a toast when Omarchy has a release (tap to open) and Update > Omarchy in the menu open it; it shows what's new, the battery and the phone patches, asks for the PIN, shows progress with the output under Details, and ends on the result. The run is a systemd user unit, so it survives the shell restart at its end.

## 6. Release bundles and the installer

For a first install, or to recover a phone that won't boot. One bundle per device per release.

**What exists (2026-10-13).** Built and tested offline; the first real use is [REINSTALL-RUNBOOK](../device/cheetah/REINSTALL-RUNBOOK.md):

| Piece | Where | Notes |
| --- | --- | --- |
| aarch64 packages without a phone | `tools/build-packages` | makepkg in an Arch Linux ARM container (the official tarball, MD5 and signature checked against the pinned build key, `tools/lib/alarm.sh`) under the laptop's qemu-user; signs with `keys/` into `out/repo/aarch64`. Mesa, Hyprland and libcamera take hours this way, so they're built only when named, once per version |
| The shared root image | `tools/build-rootfs`, `rootfs/` | pacstrap in that container: `rootfs/packages.base`, then Omarchy's own `install/omarchy-base.packages` (minus `rootfs/packages.omarchy-skip` and what isn't built for aarch64), then `rootfs/packages.omamobile` and the device package, the `[omamobile]` repository itself, the kernel modules of the current build, and `patches/omarchy`. Output `out/omamobile-rootfs-<date>-aarch64.img.zst`, `.sha256` and a `MANIFEST` with every package version (also inside, `/usr/share/omamobile/rootfs/MANIFEST`) |
| First boot | `packages/omamobile-firstboot` | reads `/etc/omamobile/firstboot.conf` (data, never sourced), creates the user, SSH keys, ufw with SSH on `usb0`, runs Omarchy's own setup leaves (skipping four that don't fit a phone), SDDM autologin and the lock, Omarchy's and our user setup, then deletes the file |
| Growing the root | `initramfs/init` (`grow_root`) | `resize2fs` before mounting, only for a file system labelled `omamobile-root` smaller than its partition; `omamobile.noresize` skips it |
| The installer | `tools/install-phone` (the bundle's `install`) | as described below, steps 1, 3-5; the factory-image step (2) is `tools/phone-provision` after the first boot instead |
| The bundle | `tools/make-bundle` | `out/bundle/omamobile-<codename>-<date>/` as below, `SHA256SUMS.sig` from `keys/` |
| Per-phone data | `tools/phone-provision` | the extract tools on the user's factory image, pushed over USB SSH, plus the phone's own backup (modem board id and NV checksums, camera data, settings, home screens, fingerprint state only with `--fingerprint`) |

Decisions taken in building it: the device packages are **installed** in the image, not only cached (one device so far; a first boot without a package step has one less way to fail; §12 decision 6 stays open for the second device); the image's `[omarchy]` is Omarchy's **edge** channel, the only one with aarch64 builds of `omarchy`, `omarchy-settings` and `quickshell`; the image is per device (its `MANIFEST` names the device, and the installer refuses a mismatch).

The design:

```
omamobile-<codename>-<version>/
  README.txt                        what it is, what it touches, how to go back to stock
  install                           the one-command installer (bash, laptop side)
  device.conf                       the descriptor the images were built from
  images/boot.img init_boot.img vendor_boot.img vendor_kernel_boot.img dtbo.img
  MANIFEST                          kernel release, package versions, both repositories' commits
  SHA256SUMS  SHA256SUMS.sig        checksums of everything above, signed
omamobile-rootfs-<version>-aarch64.img.zst   shared by every device, downloaded once
  .sha256 .sig
```

**The shared root image** is an ext4 Arch Linux ARM root with Omarchy, the `[omamobile]` repository configured and its keyring, `linux-omamobile`, `omamobile-shell` and every device package in the package cache but **none installed**. It is built by `pacstrap` in an aarch64 container. It contains no Google file. Its size is a few GB, flashed sparse; the first boot grows it to the whole `userdata` partition (`resize2fs` in the initramfs, before mounting).

**`install`** (`tools/install-phone`), run on the laptop with the phone in fastboot:

1. Checks the bundle's signature and checksums, and that `fastboot getvar product` is this bundle's codename, the bootloader is unlocked, and both slots carry the same bootloader version. Any doubt stops it.
2. Asks for the user's factory zip, checks its `board=`, and runs the device's firmware and chroot extraction on the laptop.
3. Makes a **local copy** of the root image and writes into it, with `debugfs -w` (no root needed): the extracted firmware, the user's SSH public key, `/etc/omamobile/boot-update.conf` with `AUTO_WRITE=yes`, and a first-boot marker naming the device package to install. Nothing extracted leaves the laptop except into the user's own phone.
4. Shows exactly what it will write (`BOOT_IMAGES` with `_b`, and `userdata`), says that `userdata` is erased (stock Android needs a factory reset if it is ever booted again), and waits for the user to type the codename.
5. Runs only `fastboot flash <image>_b images/<image>.img` for each `BOOT_IMAGES` entry, `fastboot flash userdata <root image>`, `fastboot --set-active=b` and `fastboot reboot`. The partition names come from the descriptor and are checked against its `UFS_RW` again; the script has no code path for any other partition, no `-w`, no `--slot`, no `flashing` command.
6. On first boot, `omamobile-setup-system` installs the device package named by the marker (from the package cache, offline) and reminds the user to take the read-only backup of `efs`, `efs_backup`, `modem_userdata`, `persist` and `modem_a/b` ([MODEM-PLAN](../device/cheetah/MODEM-PLAN.md) M0.1) before any radio work.

`bin/omamobile flash` stays the developer's tool; `install` is for everyone else and is deliberately narrower.

## 7. Versions and tags

| Thing | Version | Where it is recorded |
| --- | --- | --- |
| Kernel | `uname -r`: `7.3.0-rc6-omamobile-<n>-g<sha>`; pkgver `7.3.0rc6.r<n>.g<sha>` | tag `omamobile-int-<YYYY.MM.DD>` in linux-omarchy-mobile |
| Packages | each package's `version` file, bumped in the commit that changes it (CONTRIBUTING §6.3) | the package |
| Descriptor | none of its own; a change to it is a device-package change and bumps that package | `omamobile-device-<codename>` |
| Bundle | calendar version `<YYYY.MM.DD>`, with `.1`, `.2` for a respin the same day | tag `release-<YYYY.MM.DD>` in this repository, covering every device released that day; `MANIFEST` pins the kernel tag, every package version and both commits |

A release never contains a kernel or package that isn't in `[omamobile]` at the same moment, so a bundle install and `omarchy update` meet at the same versions.

## 8. The kernel branches

- **`omamobile-int`** is the snapshot the phone runs and that releases are built from. Before each release it is published as a snapshot (`tools/publish-kernel omamobile-int`) and tagged `omamobile-int-<date>`, so anyone can rebuild a release's kernel.
- **`omamobile`** stays the curated, upstream-ready series. Before each release, a curation pass moves whatever has passed on the phone since the last one out of `omamobile-int` into `omamobile` as clean commits (the `omamobile-next` process in [KERNEL-PUBLISH](../device/cheetah/KERNEL-PUBLISH.md)), and `omamobile` is published. The release notes list how many commits `omamobile-int` still carries beyond `omamobile`; that number should shrink over time.
- Board DTS files for new devices land in the device's topic branch first, then in both branches as above.

## 9. Reproducibility and CI

**What step 1 showed.** Given the same kernel build and initramfs, `bin/omamobile images` is deterministic: two runs and the old and new code all gave identical checksums for `boot.img`, `init_boot.img`, `vendor_boot.img`, `vendor_kernel_boot.img`, `dtbo.img`, `dtb.img`, both DTB copies and `Image.lz4`. `initramfs` is deterministic in content but not in bytes: its cpio carries mtimes and inode numbers. Packed with fixed mtimes and `cpio --reproducible`, old and new code gave the same archive.

**To make whole releases reproducible:**

- Pin the build container by digest (`docker/Dockerfile` `FROM ubuntu:24.04@sha256:…`), and record the clang version in `MANIFEST`.
- Set `KBUILD_BUILD_TIMESTAMP`, `KBUILD_BUILD_USER`, `KBUILD_BUILD_HOST` and `SOURCE_DATE_EPOCH` from the tagged commit.
- Pack the initramfs with fixed mtimes, a sorted list and `cpio --reproducible`, and without `authorized_keys` (§5.2).
- Pin the Alpine minirootfs and APKs by checksum (the minirootfs is already pinned; `alpine-fetch.py` should pin the APKs too).
- The clang comes from a 400 GB LineageOS checkout today. A release build should use the AOSP clang prebuilt alone, or the distro's LLVM in the container; either changes the kernel's bytes once, so it's a decision (§11), made before the first release.

**CI** (GitHub Actions, or any runner with docker):

| Job | When | What |
| --- | --- | --- |
| lint | every push | `bash -n` and shellcheck on `bin/` and `tools/`, each package's `test/all`, descriptor checks for every `device/*/device.conf` |
| kernel | kernel tag, or nightly on `omamobile-int` | build `Image`, modules and all DTBs in the container; `dt-validate` (`CHECK_DTBS=y`) on each board DTB |
| images | after kernel | `DEVICE=<each> bin/omamobile images`; build twice and compare checksums |
| packages | release | `linux-omamobile` packed from the kernel job; aarch64 packages on an arm64 runner in an Arch Linux ARM container, or on the phone as today |
| bundle | release | bundle and root image, **unsigned** |

CI never holds the signing key and never sees a factory image. The maintainer downloads the CI artifacts, rebuilds locally and compares checksums, then signs (§10). Nothing CI produces contains a Google file.

## 10. Signing

- One key signs everything today: packages, the repository database, and (new) each bundle's `SHA256SUMS` and the root image's checksum. Its private half lives in `keys/` on the maintainer's machine (gitignored); its public half ships in `omamobile-keyring`, and the bundle `install` script carries the fingerprint.
- `tools/repo-build` already signs packages and the database. A `tools/release-sign` signs bundles the same way (`gpg --detach-sign` with `keys/gnupg`).
- Rotation and revocation go through `omamobile-keyring`'s `omamobile-trusted` and `omamobile-revoked`, as Arch's keyring does. A separate release key, or subkeys with the primary offline, is worth doing before the repository goes public (§11).
- Never commit, paste or upload a key, a passphrase or `keys/`. CI gets no key.

## 11. Release checklist

Per release, in order. Stop at any failed step.

1. **Kernel:** every subsystem branch meant for the release merged into `omamobile-int`; it boots on each released device, with `uname -r` and the result in that device's NOTES.
2. **Curate:** move what has passed into `omamobile` (§8); publish `omamobile` and the `omamobile-int` snapshot; tag `omamobile-int-<date>`.
3. **Build** the kernel from the tag in the pinned container, release `LOCALVERSION`; pack `linux-omamobile`.
4. **Images** for every device: `DEVICE=<codename> bin/omamobile images`, twice, checksums equal.
5. **Packages:** every changed package has a bumped `version`, its `test/all` passes, `tools/build-packages` builds and signs them and the database on the laptop (`tools/repo-build` on a phone only as an opt-in shortcut).
6. **Test install** on each released device, attended: the bundle's `install` on a phone whose stock slot A and backups are known good; first boot, device package installed, `omarchy update` clean, then a package-hook kernel update and a reboot into it.
7. **Identifiers:** grep the bundle, the root image's file list, `MANIFEST` and the release notes for MAC addresses (`[0-9a-f]{2}(:[0-9a-f]{2}){5}`), serial numbers, SSIDs and account names; confirm no file from a factory image is in any artifact.
8. **Sign** the bundles and the root image; verify the signatures from a clean keyring.
9. **Tag** `release-<date>` in this repository; publish the repository, the bundles and the notes (what changed, the kernel tag, `omamobile-int` minus `omamobile`, known issues, how to go back to stock).

## 12. Decisions for the human

1. **Where the kernel package is built:** cross-compiled in the x86 container and packed on the laptop (fast, matches today's builds), or natively on an aarch64 builder.
2. **Toolchain for releases:** keep the LineageOS clang, switch to the standalone AOSP clang prebuilt, or the distro's LLVM in the container.
3. **The package hook on development phones:** opt-in by `AUTO_WRITE=yes` (proposed), or on by default with `bin/omamobile push` turning it off.
4. **Signing key:** keep the one key for bundles too, or create a release key (or subkeys) before the repository is public.
5. **CI host** for aarch64 packages: GitHub's arm64 runners, another arm64 machine, or the phone as today.
6. **Root image:** device packages in the cache and installed on first boot, or one root image per device. The first build (§6) installs them, so its image is per device; revisit with the second device.
7. **Bundle hosting:** GitHub Releases on this repository, or next to the `[omamobile]` repository.
8. **Second device:** which Pixel proves the model (a gs201 sibling such as panther needs mostly a DTS; a gs101 phone also tests the different boot image set).
