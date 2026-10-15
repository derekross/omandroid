<!-- SPDX-License-Identifier: MIT -->
# mesa (Omarchy Mobile build)

Arch Linux's Mesa 26.2.4 package, rebuilt for the Pixel 7 Pro and the
Pixel 6 Pro. Mali-G710 (gs201) and Mali-G78 (gs101, with upstream's G68
next to it) are added to Panfrost's model table. Without an entry, Mesa
rejects the GPU (for the G710 even when Panthor is loaded): the Gallium
driver bails out on an unknown model, and PanVK returns
`VK_ERROR_INCOMPATIBLE_DRIVER`. See `device/cheetah/GPU-PLAN.md`, section 4.

| File | What it is |
| --- | --- |
| `PKGBUILD` | Arch's `mesa` PKGBUILD at tag `1-26.2.4-1`, trimmed for this phone (see its header) |
| `0001-panfrost-add-Mali-G710-to-the-model-table.patch` | The G710 entry, in `git format-patch` form |
| `0002-panfrost-don-t-make-image-BOs-private-on-panthor.patch` | Image BOs are always shareable: panthor can't export a BO private to one GPU VM, and GTK 4 exports GL textures made without `PIPE_BIND_SHARED` (GNOME Snapshot crashed after photos with "drmPrimeHandleToFD() failed (err=22)") |
| `0003-panfrost-Advertise-Mali-G68-support.patch` | Upstream's G68 entry (93d2c51f, Markuss Broks), backported to 26.2.4 |
| `0004-panfrost-add-Mali-G78-to-the-model-table.patch` | The G78 entry (Valhall v9, run as the G57). Gallium only: PanVK still refuses the G78 |
| `UPSTREAM.md` | Merge request text for upstream Mesa |
| `.SRCINFO` | `makepkg --printsrcinfo` output |

## What differs from Arch Linux ARM's `mesa`

- The patches above.
- `arch=(aarch64)`.
- Only drivers this phone can use:
  - Gallium `panfrost` (which brings `kmsro`), `llvmpipe`, `softpipe` and `zink`.
  - Vulkan `panfrost` (PanVK).
  - The `device-select` and `anti-lag` layers.
- Only three split packages: `mesa`, `vulkan-panfrost` and
  `vulkan-mesa-implicit-layers` (which `vulkan-panfrost` depends on).
- `pkgrel=1.3`. makepkg only accepts `integer[.integer]`, so the Omarchy Mobile
  marker is in Mesa's version string instead: `26.2.4-arch1.1.3.omamobile3`.
  That string shows up in `GL_VERSION` and in the `libgallium-*.so` file name.

The other ALARM split packages (`vulkan-swrast`, `opencl-mesa` and so on)
don't depend on `mesa`'s exact version. They can stay installed at
`1:26.2.4-1`.

## Status

The PKGBUILD has **not been built yet**. It will be built natively on the
phone. What has been checked:

- The tarball's checksums and PGP signature (key `57551DE1…` from Arch's
  `keys/pgp`).
- `makepkg --nobuild` with `prepare()`. The patch applies cleanly to the
  release tarball (`patch --dry-run` passes as well).
- Every meson option name exists in 26.2.4's `meson.options`.

A full `meson setup` has not been run, because this host lacks meson and
libclc.

## Building on the phone

As a normal user in the Arch root, with internet:

```sh
sudo pacman -S --needed base-devel
gpg --recv-keys 57551DE15B968F6341C248F68D8E31AFC32428A6   # Eric Engestrom, signed 26.2.4
cd packages/mesa
makepkg -s                       # pulls the makedepends listed in the PKGBUILD
sudo pacman -U mesa-1:26.2.4-1.3-aarch64.pkg.tar.zst \
  vulkan-panfrost-1:26.2.4-1.3-aarch64.pkg.tar.zst \
  vulkan-mesa-implicit-layers-1:26.2.4-1.3-aarch64.pkg.tar.zst
```

Until Panthor exposes `/dev/dri/renderD128`, nothing changes visibly:
`kmsro` finds no render node and Mesa keeps using `kms_swrast`.

To stop `pacman -Syu` from replacing the build with ALARM's `mesa`, add this
line to `/etc/pacman.conf`:

```
IgnorePkg = mesa vulkan-panfrost vulkan-mesa-implicit-layers
```

Alternatively, publish the build in Omarchy Mobile's own repo listed ahead of
`[extra]`. Rebase on each new ALARM Mesa release:

1. Take the new Arch tag.
2. Re-apply the trimming.
3. Check that the patch still applies.

Drop the G710 and G78 patches once upstream Mesa ships their entries.

On the Pixel 6 Pro (raven) the G78 patch runs GLES 3.1 with Hyprland on
top. It needs the kernel's AArch64 page-table quirk for gs101 in
Panfrost, or every fragment job faults.
