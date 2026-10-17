# omamobile-device-cheetah

Omarchy Mobile's system support for the Google Pixel 7 Pro (codename cheetah, Tensor G2 / gs201). It sits beside Omarchy's own packages, the way `omarchy-mac` adds Apple Silicon support.

| Path | Installs to | What it does |
| --- | --- | --- |
| `bin/omamobile-boot-success` + `vendor/systemd/system/omamobile-boot-success.service` | `/usr/bin`, `/usr/lib/systemd/system` (enabled by a vendor wants link) | Marks the booted A/B slot successful after each healthy boot, as Android's boot-control service does |
| `bin/omamobile-wifi-start` + `vendor/systemd/system/omamobile-wifi.service` + `vendor/modprobe.d/omamobile-wifi.conf` | `/usr/bin`, `/usr/lib/systemd/system` (enabled by a vendor wants link), `/usr/lib/modprobe.d` | Brings up Wi-Fi at boot (PCIe controller, then brcmfmac), skipping it on the next boot if a bring-up never finished. The modules are blacklisted for autoload so only this guard loads them. The chip's buffers come from a restricted DMA pool (DT): that contains driver bugs, not hostile firmware |
| `bin/omamobile-camera-start` + `vendor/systemd/system/omamobile-camera.{service,timer}` + `vendor/modprobe.d/omamobile-camera.conf` | `/usr/bin`, `/usr/lib/systemd/system` (the timer enabled by a vendor wants link), `/usr/lib/modprobe.d` | Loads the camera drivers 40 s into the boot (PMIC supplies, the four sensors and their lenses, the DC-PHY and CSIS), skipping them on the next boot if a load never finished. The modules are blacklisted for autoload so only this guard (or a runbook) loads them; libcamera and PipeWire then find the four cameras. `vendor/udev/rules.d/70-omamobile-dma-heap.rules` lets the video group (and the seat's user) use the general DMA-BUF heaps, which libcamera's software ISP needs in a user session. `wireplumber/50-omamobile-cameras.conf` (installed to `/usr/share/wireplumber/wireplumber.conf.d`) names the cameras (Main, Ultrawide, Telephoto, Front; Main first) and keeps the raw CSIS video nodes out of PipeWire |
| `src/omamobile-bootctl.c` | `/usr/bin/omamobile-bootctl` | Reads and updates the slot flags in `devinfo`. Ported from Google's Apache-2.0 Pixel boot-control code; only ever changes bytes 0x30-0x37, verified |
| `src/reboot-to.c` | `/usr/bin/reboot-to` | Reboot with a reason the bootloader understands. One source, inside this package, also linked into every initramfs |
| `bin/omamobile-setup-system` | `/usr/bin` | System setup: UTF-8 locale, timezone, NTP (Omarchy lifecycle `setup-system`) |
| `vendor/udev/rules.d/10-omamobile-storage.rules` | `/usr/lib/udev/rules.d` | Continues the initramfs storage guard: Android partitions stay hidden and the whole-disk node unusable |
| `vendor/systemd/network/20-usb-gadget.network` | `/usr/lib/systemd/network` | USB networking to a host (172.16.42.1) |
| `vendor/NetworkManager/conf.d/10-omamobile-usb.conf` | `/usr/lib/NetworkManager/conf.d` | Keeps NetworkManager off the USB link |
| `vendor/systemd/logind.conf.d/10-omamobile-power-key.conf` | `/usr/lib/systemd/logind.conf.d` | A short power press opens the system menu instead of powering off |

`./build [DIR]` compiles the C tools, `./install DESTDIR [DIR]` stages everything and enables nothing, and `test/all` runs the package tests.

Licences: MIT, except `omamobile-bootctl`, which is Apache-2.0 and keeps Google's notice.
