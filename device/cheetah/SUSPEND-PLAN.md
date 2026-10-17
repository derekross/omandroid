<!-- SPDX-License-Identifier: MIT -->
# Suspend plan: s2idle first, then SYS_SLEEP (cheetah / gs201)

Status (2026-10-12, evening): **S0, S1 and S2 passed on the phone** (section 8).
S3 is next. Kernel branch `omamobile-suspend` in `~/android/mainline/wt-suspend`
(off `omamobile`, with `omamobile-acpm` merged in, because the RTC sits on
ACPM), built into `~/android/mainline/out-suspend`; head `a7e5e1d95` adds the
gs201 UFS `spm_lvl` 5 default. Wi-Fi D3 is branch `omamobile-wifi-d3` in
`~/android/mainline/wt-wifi-d3` (now on top of `omamobile-int`), built into
`~/android/mainline/out-wifi-d3`. Userspace pieces (sleep hook, `sleep.conf`
drop-in) are new, inert files in `omamobile-device-cheetah` (section 6.2).
The runbook is section 8: S3 (RTC and the UFS default), S4 (C2), S5 (Wi-Fi
D3), S6 (AoC and sensors), S7 (battery), S8 (userspace on).

**Lessons from S1/S2 that every later stage keeps:** UFS at `spm_lvl` 5
(runtime `echo 5` until the kernel default is in, then just check it); Wi-Fi
unloaded (`modprobe -r brcmfmac_wcc brcmfmac pcie_gs201`) unless the stage is
testing D3; wake with Volume Up, not Power; log to UFS and run detached.

**Goal.** Android-like standby. Screen off means sleep. The power key, USB,
Bluetooth, Wi-Fi and (later) the modem wake the phone, and the drain is very
low. That takes two kinds of sleep:

1. **s2idle** ("freeze"): userspace frozen, devices suspended, CPUs in their
   deepest cpuidle state (C2), and the SoC and every PMIC rail left on. This
   is the generic Linux mechanism. It needs no secure-world sequence, only
   drivers that suspend and resume cleanly and interrupts that can wake.
   It saves little power, but it proves the drivers, the wake sources and
   the userspace policy. Everything later builds on it.
2. **SYS_SLEEP** (what stock Android does): the PMU and the APM firmware
   power the SoC down, put DRAM in self-refresh and drop PWREN, so about 30
   PMIC rails switch off. Entered through PSCI SYSTEM_SUSPEND after a
   table-driven PMU sequence. This is where the tens-of-mA standby comes
   from.

Path abbreviations (as in the other plans):

- `L/` = `~/android/mainline/wt-suspend` (our kernel, this branch)
- `G/` = `~/android/omamobile/out-kernel/google/gs-6.1/private` (Google's GPL gs201 kernel)
- `GS/` = `G/google-modules/soc/gs`
- `GD/` = `G/devices/google/gs201/dts`
- `R/` = `~/android/reference/cheetah-stock-cp2a-root` (stock live DT, dmesg, interrupts)
- `P/` = this repository

---

## 1. Firmware: what PSCI offers, and the trap in "mem"

- **PSCI 1.1, platform-coordinated.** Our boots print "PSCIv1.1 detected in
  firmware" and "MIGRATE_INFO_TYPE not supported" (`P/logs/acpm-kernel.log`,
  `R/dmesg.txt:94-98`). Stock's DT is `psci { compatible = "arm,psci-1.0";
  method = "smc"; }` (`R/live.dts:2593-2596`).
- **CPU_SUSPEND (C2) works with the PMU hint.** `arm,psci-suspend-param =
  <0x10000>` (power-down, level 0) on all cores. The firmware accepts it
  only if CPU_INFORM(cpu) = 1 was written first (POWER-PLAN 1.3-1.4). C2
  passed on the phone with overlays (POWER-PLAN section 7). It is not in
  the default DTB yet (`omamobile-power-on` not merged).
- **SYSTEM_SUSPEND is implemented.** Stock enters every system sleep through
  the generic PSCI SYSTEM_SUSPEND (`cpu_suspend(0, psci_system_suspend)`,
  SMC `0xC400000E`, entry = `__pa(cpu_resume)`, context 0), with no vendor
  PSCI code. Google's boot metrics also time EL3's
  `psci_system_suspend` (`GS/drivers/soc/google/debug/pixel-boot-metrics.c:141-148`).
  External bring-up reports agree that the firmware advertises it.
- **The trap.** With SYSTEM_SUSPEND advertised, mainline's
  `psci_init_system_suspend()` (`L/drivers/firmware/psci/psci.c:582-593`)
  registers `psci_suspend_ops`. `suspend_set_ops()` then makes `deep` the
  default (`L/kernel/power/suspend.c:228-238`), so `/sys/power/mem_sleep`
  reads `s2idle [deep]`. `systemctl suspend` and `echo mem > /sys/power/state`
  would then call SYSTEM_SUSPEND **without** stock's PMU preparation: no
  CPU_INFORM = 4, no wake masks, no CMU save, no pad-retention release on
  exit. The likely outcome is no wake, or a reset.
  - **Fix (kernel):** `mem_sleep_default=s2idle` in the DTB bootargs (commit in
    section 7). After that, "mem" means s2idle until SYS_SLEEP exists.
  - **Fix (userspace, belt and braces):** `MemorySleepMode=s2idle` in a
    `systemd/sleep.conf.d` drop-in of `omamobile-device-cheetah`.
  - **Until either is in:** use only `echo freeze > /sys/power/state`
    (`rtcwake -m freeze`). Never `mem`.

## 2. s2idle on gs201: what happens

Without cpuidle states (int before `741b6ed1d`), in s2idle the CPUs sit in WFI
(`L/kernel/sched/idle.c:196-199`, `default_idle_call()`), and the last CPU
freezes the tick and timekeeping. The arch counter is
`CLOCK_SOURCE_SUSPEND_NONSTOP`, so time stays right.

With the C2 overlays (or `omamobile-power-on`), each plain DT idle state also
gets `enter_s2idle` (`L/drivers/cpuidle/dt_idle_states.c:38`). s2idle then
uses C2 directly. **Upstream bug for this path, fixed on this branch:**
`exynos-pmu`'s `suspend_noirq` sets `sys_insuspend`, and from then on its CPU
PM notifier stops writing the CPU_INFORM hint
(`L/drivers/soc/samsung/exynos-pmu.c:326-349, 557-583`). That is right for a
firmware system suspend, where the hint must stay SLEEP (stock's cpupm does
the same with `system_suspended`, `GS/drivers/soc/google/exynos-cpupm.c:991-993`).
It is wrong for s2idle: the firmware then refuses every C2 entry (POWER-PLAN
1.3), each refusal returns at once, and the CPUs spin on PSCI calls until
wakeup. Commit `8d820bc0a` keeps the hint when the target is s2idle. The
proof is `state1/rejected` staying a small fraction of `state1/usage` across
a freeze (stage S4).

**What s2idle can't save.** PWREN stays high and the SoC never leaves its
active power state, so the rails, DRAM refresh and the always-on blocks keep
their awake cost. External numbers on this phone with mainline: about
274-286 mA from the battery in s2idle, display off, Wi-Fi unloaded, against
about 50 mA (later 32-45 mA) in SYS_SLEEP. Expect s2idle to beat
"screen off, awake" only by what the CPUs, devices and timers stop drawing.
Measure it (section 8, S7) and don't sell it as Android standby.

## 3. Wake sources

| Source | Line | Our state | Wakes s2idle? |
|---|---|---|---|
| Power key | gpa10-0 (`PMIC_PWRON_OD_L`), ALIVE EINT, GIC SPI 42 | `gpio-keys`, `wakeup-source` (`gs201-cheetah.dts`) | Yes. `gs101_wkup_irq_set_wake` only records the mask bit; the SPI stays live |
| Volume down / up | gpa10-1 (SPI 43) / gpa8-5 (FAR_ALIVE) | same node | Yes. **Use Volume Up to wake in tests**: Hyprland binds the power key to lock and screen off (`omamobile-keys`) |
| RTC alarm | S2MPG12 RTC over ACPM channel 2, PMIC interrupt in-band (I3C IBI), raised by SYSREG_VGPIO2AP as GIC SPI 72, level high (`R/live.dts:13103-13107`; `R/interrupts.txt`: "GICv3 104 Level s2mpg12-irq") | **New on this branch**, node disabled (section 7) | Yes once enabled. `rtcwake` needs it |
| Fuel gauge | gpa9-3, level low, shared | `max17042_battery` calls `enable_irq_wake` unconditionally in suspend | Yes. **Can cause unexpected wakes or an immediate abort** if the alert line is held low |
| Bluetooth host wake | gpa6-6 (FAR_ALIVE), rising edge | `hci_bcm` arms it in suspend | Yes: BT traffic wakes the phone |
| Wi-Fi host wake | gpa6-1 (FAR_ALIVE) | **no consumer**: not requested by `pcie-gs201` or brcmfmac | No |
| USB / Type-C | TCPCI MAX77759 on gpa9-7 (stock enables wake unconditionally, `tcpci_max77759.c:2420-2433`) | TCPC blacklisted, node disabled (USB-C-PLAN) | No. USB plug-in can't wake yet |
| Touch | gpa7-0, level low | `synaptics_tcm`, not wake-armed | No (stock doesn't wake on touch either, except AOD gestures) |
| Modem | CP2AP_WAKEUP gpa8-3 | no modem driver | Later |

### 3.1 The Volume Up wake in S2 (spurious or not)

S2's first freeze resumed 3 s in on IRQ 106 (gpio-keys, `KEY_VOLUMEUP`,
gpa8-5) with nobody touching the phone. The hypothesis was a missing
power-down pull. **The captures rule that out:**

- gpa8 is a FAR_ALIVE bank (`pinctrl@180E0000`, gpa6/7/8/11). In Google's
  gs201 pinctrl driver every ALIVE and FAR_ALIVE bank is `bank_type_7`,
  `fld_width = { 4, 1, 4, 4 }`: CON, DAT, PUD, DRV only, **no CON_PDN or
  PUD_PDN registers** (`GS/drivers/pinctrl/gs/pinctrl-gs201.c:67-89`).
  Mainline's `exynos850_bank_type_alive` is the same. Alive pads keep their
  normal configuration in every sleep state, so there is no power-down pull
  to set. Stock's `samsung,pin-con-pdn`/`pin-pud-pdn` on FAR_ALIVE pins
  (`bt-host-wake`, `wlan_host_wake`, `gps-host-req`, `R/live.dts:3118-3226`)
  have no register to land in.
- Stock configures `key-volup` exactly as we do: function EINT, `pin-pud = <0>`,
  `pin-drv = <0>` (`R/live.dts:3159-3165`), against `pin-pud = <3>` for
  Power and Volume Down (`R/live.dts:2971-2985`). The line must have an
  external pull-up; with none, the key would chatter while awake too.
- On every suspend, s2idle included, mainline's `gs101_pinctrl_suspend()`
  switches each wake-up EINT bank's filter from digital to analog, and
  resume switches it back (`L/drivers/pinctrl/samsung/pinctrl-exynos.c:909-999`).
  Stock does the same (`GS/drivers/pinctrl/gs/pinctrl-exynos.c`,
  `exynos_pinctrl_suspend()`: "Setting Analog Filter"). The analog filter
  passes shorter pulses than the digital one, so a glitch that the awake
  phone ignores can wake a sleeping one. That is the only difference
  between awake and asleep for this pin, and it applies equally to stock.

**Proposal: no DT change.** A pull-up on gpa8-5 would deviate from stock and
can't fix a PDN state that doesn't exist. Instead, S4's 20-cycle loop counts
wakes by IRQ. If Volume Up wakes recur with nobody touching the phone:

1. read `/sys/kernel/debug/pinctrl/180e0000.pinctrl*/pins` for gpa8-5
   (CON/DAT/PUD) awake, and the gpio-keys state (`evtest --query`), to
   confirm the line idles high;
2. then a kernel experiment (its own branch, one boot): keep the digital
   filter in suspend-to-idle (`gs101_pinctrl_suspend()` switches to analog
   only when `pm_suspend_target_state != PM_SUSPEND_TO_IDLE`). The digital
   filter's clock runs in s2idle; analog is only needed where it stops
   (SYS_SLEEP). If the spurious wakes stop, that is an upstream patch;
3. only if a scope or the pin readback shows a floating line: a DT pull-up,
   as a test overlay first.

Stock's PMU wake set for SYS_SLEEP, for later: `WAKEUP_INT_EN` (PMU `0x3944`)
= `0x1001f0bf` and `WAKEUP2_INT_EN` (`0x3964`) = `0x1f0`. That covers the RTC,
EINT and EINT_FAR, the AoC mailbox, PCIe, USB_REWA, and the APM/AoC/debug
mailboxes (`GD/gs201.dtsi:382-430`, `R/live.dts:12844-12872`). In s2idle these
PMU registers don't matter: the GIC is awake.

## 4. Drivers across s2idle (our integration kernel)

From an audit of `omamobile-int` (head `907128673`), with Google's handling for
comparison.

| Driver | What happens in s2idle | Risk / action |
|---|---|---|
| **UFS** (`ufs-exynos.c`, gs201 data) | Core default `spm_lvl` = 3: device SLEEP, link Hibern8. The gs101/gs201 vendor `suspend` (`gs101_ufs_suspend`, `L/drivers/ufs/host/ufs-exynos.c:1806-1810`) writes `HCI_GPIO_OUT = 0`, which **asserts the device reset**. Then `phy_power_off` if the link isn't active. Resume does `phy_power_on`, SMU and FMP; nothing deasserts reset, so the Hibern8 exit fails and the core's link recovery resets the host and device (`exynos_ufs_dev_hw_reset`). Stock uses `spm_lvl` 5 (device powered down, link off) for system suspend (`GS/drivers/ufs/ufs-exynos.c:702-705`), so its resume is always a full re-init, which matches the reset. | **Medium, root FS.** S1 at level 3: the first Hibern8 exit failed and a host reset recovered. S2 at level 5 (runtime `echo`): clean. **Kernel default on `omamobile-suspend` (`a7e5e1d95`)**, as Google's driver; stage S3 checks that `spm_lvl` reads 5 after boot. Until that kernel runs, `echo 5 > spm_lvl` before every freeze. `rpm_lvl` stays 3 (stock leaves it too). `sync` before every test |
| **Display** (simpledrm on ABL's framebuffer; `gs_dsim_cmd` + `panel-samsung-s6e3hc4` for commands and `bl_power`) | simpledrm's suspend only disables its software CRTC. DECON0 → DSIM0 → panel keep running on ABL's setup plus our `TRIG_CON` write from init (`initramfs/init`, from `SIMPLEFB_TRIG_CON`), all powered (`pd_ignore_unused`). The panel driver has no PM ops. | Low for s2idle: the picture stays (a frozen picture is the visible sign of a freeze). Userspace blanks with `bl_power = 4` before sleeping and unblanks on a user wake. For SYS_SLEEP the DPU domain powers off: TRIG_CON is lost, the panel needs its full init (DISPLAY-PLAN D3b/D4c), and with stock's PWREN rails off a powered display domain stopped the wake in external tests |
| **GPU** (Panthor, `clk_acpm`) | System sleep uses `pm_runtime_force_suspend` even though our udev rule pins runtime PM on (`P/packages/omamobile-device-cheetah/vendor/udev/rules.d/20-omamobile-gpu-always-on.rules`). `panthor_device_suspend` stops the scheduler, firmware and MMU, then devfreq, then `clk_disable`. `clk_acpm` has no gate ops, and there is no G3D PD driver, so the GPU stays powered. Resume reboots the MCU firmware. | Medium: the first run of this path. S2 watches for "FW slow reset" or job timeouts after resume |
| **Wi-Fi** (`pcie_gs201` + brcmfmac) | brcmfmac `.suspend` sends D3_INFORM and waits 2 s for D3_ACK; on timeout it returns **-EIO and aborts the suspend** (`pcie.c:3310-3335`). **It can't work today:** our BCM4389 firmware reports shared flags `0x70050107` (version 7, no `PCIE_SHARED_USE_MAILBOX` `0x2000000`, `bcmpcie.h:103` in Google's driver), so, as in Broadcom's driver, mailbox data (D3 inform/ack, deep sleep) travels as control-ring messages (types `0x23`/`0x24`); brcmfmac writes the TCM mailbox, which this firmware ignores, and logs the replies as "Unsupported msgtype 36". Fixed on branch `omamobile-wifi-d3` (section 7). `pcie-gs201` has **no PM ops**: no PME_Turn_Off, no L2, no PERST, no PHY isolation; the link stays up and the PCI core puts the endpoint in D3hot. Host wake gpa6-1 has no consumer. | **Unload Wi-Fi for stages S1-S4** (`modprobe -r brcmfmac_wcc brcmfmac pcie_gs201`: `brcmfmac_wcc` holds `brcmfmac`, and unloading it first unbinds the device). Wi-Fi across s2idle is stage S5 (sections 6.4, 6.5); the sleep hook unloads it until then (section 6.2). External reports with mainline brcmfmac on this chip: the D3 ack arrives as a control-ring message and was missed until D3 completion was published only after the IRQ thread finished RX; even then about 2 in 20 s2idle resumes hung the firmware ("Failed to reserve space in commonring", then -EIO on every later suspend) |
| **Bluetooth** (`hci_bcm`, `samsung_tty`) | `bcm_suspend` drops BT_DEV_WAKE (gpp16-3) and arms host wake. The UART has suspend, resume and resume_noirq. | Low. For SYS_SLEEP BT_REG_ON (gpp16-2) needs stock's "previous" PDN state, or the chip loses power (BLUETOOTH-PLAN 367) |
| **AoC** (`aoc_core`, `aoc_channel_dev`, `aoc_alsa_dev`, the `wc-mbox` mailbox; Google's driver) | The AoC is its own always-on processor and keeps running; the AP side only bookkeeps. `aoc_core_suspend/resume` count queued messages for wake-capable services. `aoc_channel_dev`'s `prepare`/`complete` send `SUSPEND_PREPARE`/`WAKEUP_COMPLETE` to `com.google.usf` (the sensor hub), so it can batch. **`wc_mbox_suspend` masks the non-waking services and calls `enable_irq_wake`**: any message on a waking service (sensor events among them) wakes the AP, and `aoc_channel_dev` then holds a 200 ms wakelock per waking message. The sensor proxy and autorotate are frozen with userspace, so a running accelerometer stream may keep waking the phone. | **Unknown; stage S6.** Keep the AoC loaded (never `rmmod`, AGENTS.md). Measure wakes by IRQ with the sensor proxy idle and with autorotate running; check that audio and all four sensors still work after resume. If sensor streams wake it, the sleep hook stops the sensor clients (not the AoC) |
| **Haptics** (`cs40l26-vibra`, ours) | No PM ops. The DSP is kept out of hibernate; `play_work` on the system workqueue could hit the suspended I2C adapter (-ESHUTDOWN) if a buzz is in flight. | Low/medium. Add a suspend that stops playback and lets the DSP hibernate (Google's `cs40l26.c` PM ops) |
| **Touch** (`synaptics_tcm`, ours) | No PM ops, no sleep command. The controller keeps scanning; its IRQ is masked as suspended and the pending ATTN replays on resume. | Power drain; add suspend = TouchComm sleep (or hold reset as stock does at screen off), resume = wake + re-read |
| **Torch** (`leds-lm3644`) | `LED_CORE_SUSPENDRESUME`: off in suspend, restored on resume. | OK |
| **Fuel gauge** (`max17042_battery`) | Disables its IRQ and arms it for wake. | See section 3. Charger/TCPC are blacklisted |
| **ACPM, `clk_acpm`, mailbox** | No PM ops; transfers are polled, the mailbox uses no IRQ. Stock also sends **no** ACPM message around suspend; it only reads the FlexPMU counters (`GS/drivers/soc/google/exynos-pm.c:221-229`). | Low. The S2MPG12 core reads the PMIC over ACPM in its resume path, which works because ACPM is polled |
| **Watchdogs** (`s3c2410_wdt`) | Inactive (`tmr_atboot=0`). Suspend saves and stops, resume restores. systemd doesn't hold `/dev/watchdog0` at runtime (only `RebootWatchdogSec`). | OK. The test feeder (POWER-PLAN 5.4) is frozen with userspace, and the driver stops the timer first, so it can't fire during sleep |
| **USB** (dwc3 peripheral, `phy-exynos5-usbdrd`) | `dwc3_gadget_suspend` (soft disconnect), then core exit; resume re-inits core and PHY, forced VBUS valid. | **The SSH link drops** for the whole sleep and must re-enumerate. Tests run detached and log to UFS. If USB doesn't come back, the log and the screen still tell the story |
| **I2C, SPI, USI** | `i2c-exynos5` and `exynos-usi` have noirq PM; `spi-s3c64xx` may return -EBUSY if busy. | Low |
| **MCT / arch timer** | MCT has `resume` and `tick_resume`; MISC stays powered in s2idle. | OK. In SYS_SLEEP MISC powers off and the MCT counter must be restarted on resume |
| **PMIC / RTC** (new) | `sec_pmic_suspend` arms its IRQ for wake and disables it; resume reverses. | New, stage S3 |

## 5. SYS_SLEEP (deep), for later: what stock does

Order, from Google's GPL sources:

1. `suspend_noirq`: PCIe PHY isolation for a link that is down; cpupm stops
   hinting (`system_suspended`).
2. **syscore, `exynos-pm`** (`GS/drivers/soc/google/exynos-pm.c:150-232`):
   - EINT wake masks to PMU `0x3a80/0x3a84/0x3a88` (1 = masked), from the
     pinctrl wake bitmap;
   - `WAKEUP_STAT`/`WAKEUP2_STAT` (`0x3950`/`0x3970`) = 0, then the two
     `WAKEUP_INT_EN` values above;
   - `cal_pm_enter(SYS_SLEEP)` (mode index 8; the PCIe-on HSI1ON mode 13 is
     never chosen, because the hook that registers it is commented out at
     `GS/.../pcie-exynos-rc.c:5785`): CPU_INFORM(cpu0) = **4**, save 545
     CMU registers (all MMIO), then 3 enter writes (PMU_INTR_GEN GRP2
     enable bit 0 and GRP1 pending clear, MMIO; `CLUSTER0_CPU0_INT_EN`
     `0x18061044` bit 3, SMC). Tables: `GS/drivers/soc/google/cal-if/gs201/flexpmu_cal_system_gs201.h:140-745`.
   - Every PMU_ALIVE (`0x1806xxxx`) write goes through SMC `0x82000504`;
     atomic set/clear use `offset | 0xc000` / `| 0x8000`
     (`GS/drivers/soc/google/cal-if/pmucal_rae.c:149-188`).
3. Pinctrl syscore saves CON/DAT/PUD/DRV/PDN of every bank and forces the
   sleep state.
4. PSCI SYSTEM_SUSPEND. EL3 and the APM firmware do the rest (SoC down, DRAM
   self-refresh, MIF down, PWREN low).
5. On wake, the FlexPMU AP-down counter tells a real sleep from an early
   wake: 37 exit writes (wake enables off, intr-gen pending clears, MIF
   `PwrMgmtMode` bit 31 = 0 at `0x20[8-b]40000 + 0xf23c`, bus DRCG
   re-enables, UFS MISC `0x147011b4` bit 8, **PMU `TOP_OUT` `0x3920` set
   bits 7, 9, 11-14** = pad-retention release) or 9 early-wake writes, then
   the 545 restores. Stock prints the wake reason from `WAKEUP_STAT`.

What Linux needs before that can work, in dependency order (external
bring-up experience on this phone, every item matching Google's sources):

- **PMU sleep driver**: the sequence above as a syscore driver in
  `drivers/soc/samsung/`, tables generated from Google's GPL `flexpmu_cal_*`
  headers, plus the CPU_INFORM handover with `exynos-pmu` (its
  `sys_insuspend` path is exactly the hook for this).
- **EINT wake-mask numbering is wrong in mainline for gs201.** Mainline numbers
  wake bits with a running count (`L/drivers/pinctrl/samsung/pinctrl-exynos.c:811`),
  so gpa3 gets 19, but the PMU (and Google's table) uses 20 for gpa3
  (`GS/drivers/pinctrl/gs/pinctrl-gs201.c:74-89`, EINT19 does not exist).
  Every bank from gpa3 on, FAR_ALIVE included, is one bit low. The power key
  (gpa10-0) must be bit 43, BT host wake 51, WL host wake 46. Needs a per-bank
  EINT offset in the gs201 bank data. Harmless for s2idle.
- **GPIO power-down states.** ABL leaves CON_PDN/PUD_PDN of many pins at input
  pull-down; in SYS_SLEEP that drops BT_REG_ON, the modem reset lines and
  UFS RST_N/REFCLK. Stock's DT sets only 49 PDN values and relies on the
  rest; we must set the ones we use (`samsung,pin-con-pdn`).
- **USI and CMU restore** (I2C/SPI lose their mode), **MCT restart**,
  **S2MPU CTRL0 restore** for HSI1/HSI2 (and G3D, MFC, CSIS when used),
  **UFS**: link off, PHY isolation (PMU `0x3ec8`), IOCC (SYSREG_HSI2 `0x710`),
  and on resume the FMP/SMU SMCs. Stock resumes with
  `SMC_CMD_FMP_SECURITY` `0xC2001810` then `SMC_CMD_FMP_SMU_RESUME`
  `0xC2001860` (`GS/drivers/ufs/ufs-exynos-crypto.c:139-157`); `0xC2001850`
  (SMU_INIT) is the probe-time call.
- **PMIC opmodes**: stock gives 31 rails `regulator-initial-mode = 1`
  (follow PWREN); ABL leaves them always-on. Needs S2MPG12/13 regulator
  support over ACPM (this branch adds only the S2MPG12 core and RTC).
- **Display off** before sleep (DPU and DISP domains down).
- **Never clear `MIF_ALWAYS_ON`** (externally reported to reset the phone;
  stock never writes it).
- External results to calibrate against: SYS_SLEEP entered and exited
  repeatedly, about 50 mA unplugged at first, 32-45 mA after pin PDN fixes,
  touch in reset and UFS idle Hibern8; a held sensor stream kept MIF waking
  about 28 times a second (+17 mA).

## 6. Userspace: screen off, sleep, wake

### 6.1 Policy (Android-like)

| State | What happens |
|---|---|
| Screen on, unlocked | Omarchy's idle chain unchanged (`shell/plugins/services/idle`: screensaver, then lock; the lock's blank timer turns the panel off, `patches/omarchy/0008`/`0009`). It has no suspend step |
| Power key, short press | Today: lock + panel off (`omamobile-keys short_press`). New: once the lock reports `secure` and the panel is off, request sleep (below) |
| Screen off and locked | After **`OMAMOBILE_SLEEP_AFTER_OFF`** seconds with no inhibitor: `systemctl suspend`. Start at 30 s for the first release, 10 s once S8 has run a day; never 0 (the user may still be looking for the key) |
| Woken by the user (Power) | Panel on to the PIN pad (`wake_locked`, today's path: gpio-keys reports the press after resume, Hyprland runs `omamobile-keys`) |
| Woken by Volume Up/Down | No light; the key acts as a volume key (media). Back to sleep after the grace period |
| Dark resume (RTC, BT, gauge, AoC) | Panel stays off. Back to sleep after a **5 s grace**, or when the inhibitor that justified the wake is released (an alarm's own unit, a sync job), capped at 60 s |
| Screen on, any state | Never suspend (the watcher only acts with the panel off) |

**Inhibitors** (logind `--what=sleep`, honoured by `systemctl suspend`; the
watcher checks `systemd-inhibit --list` too so it doesn't spam failed
requests):

- audio playing: `omamobile-audio` takes a `block` sleep inhibitor while a
  PipeWire sink is `RUNNING` (WirePlumber doesn't by default);
- calls (later, modem), `omarchy update`/pacman (wrap the update command),
  GNSS fixes in progress (`omamobile-gnss` holds one while a client has a
  session), and user downloads they asked to keep running;
- a toggle to disable sleep altogether (`omarchy-toggle-enabled omamobile-no-sleep`)
  for development; SSH sessions over USB never sleep the phone under the
  user's feet, so the watcher also refuses while an SSH session is open.

### 6.2 Pieces

- **logind**: `HandlePowerKey=ignore` stays (Hyprland owns the key).
  `systemctl suspend` goes through logind, so inhibitors and the sleep hooks
  apply. Never write `/sys/power/state` from the session.
- **Kernel**: `mem_sleep_default=s2idle` (in int), so even a plain `mem`
  is safe on our kernels.
- **`omamobile-device-cheetah`, new files (written, not yet in a release)**:
  - `vendor/systemd/sleep.conf.d/10-omamobile.conf`: `MemorySleepMode=s2idle`,
    `AllowHibernation=no`, `AllowHybridSleep=no`, `AllowSuspendThenHibernate=no`
    (belt and braces with the bootarg; there is no swap to hibernate to).
  - `bin/omamobile-sleep`, linked as `/usr/lib/systemd/system-sleep/omamobile`:
    - `pre`: unload Wi-Fi (`brcmfmac_wcc brcmfmac pcie_gs201`) unless
      `/etc/omamobile/wifi-keep-across-sleep` exists (create it once S5 passes)
      and no D3 failure was seen this boot; record the suspend failure count.
    - `post`: if `pre` unloaded Wi-Fi, `systemctl --no-block restart
      omamobile-wifi.service` (the hang-safe start, with its marker); log the
      wake IRQ and its name; if the suspend failed with
      `suspend_stats/last_failed_dev` = the Wi-Fi function, fall back to
      unloading Wi-Fi for the rest of the boot (`/run/omamobile/wifi-sleep-fallback`).
    - It doesn't touch the panel: the screen-off flows below have already
      blanked it, and lighting it is the key handler's job.
  - These files are inert until something calls `systemctl suspend`, which
    Omarchy hides while `suspend-off` is set. They ship in the release that
    S8 installs, with its version bump.
- **The watcher (`omamobile-sleepd`, a user service in `omamobile-shell`, to
  write for S8)**: polls every 2 s while the panel is off (`bl_power` != 0,
  or Hyprland DPMS off) and the session is locked; after
  `OMAMOBILE_SLEEP_AFTER_OFF` s, or the 5 s grace after a dark resume, with
  no inhibitor, SSH session or `omamobile-no-sleep` toggle, runs
  `systemctl suspend`. It also takes the request from `omamobile-keys`
  (`omamobile-sleepd now`, after `short_press` turned the panel off), so the
  power key sleeps at once. Polling only runs with the panel off and costs
  far less than the sleep it buys. Gate the whole service behind
  `omarchy-toggle-enabled omamobile-sleep` for the first release.
- **Omarchy's `suspend-off` toggle**: Omarchy hides Suspend in its system menu
  while the toggle is set; we set it in the first-run setup until the kernel
  can suspend (NOTES, "power button menu"). **Clear it only in stage S8**, on
  the image that ships, in the same release that adds the drop-in, hook and
  watcher. Check first that the menu's Suspend locks the session before
  sleeping (if it doesn't, waking with Power would hit `short_press`'s
  unlocked branch and lock instead of lighting the screen).
- **Wake reasons for the shell**: the hook's journal line (`omamobile-sleep:
  woken by IRQ ...`) is enough for S8; a dark-resume decision in the watcher
  reads `/sys/power/pm_wakeup_irq` the same way.

### 6.3 Wake sources (policy view)

| Source | Wakes today | Policy |
|---|---|---|
| Power key | yes | user wake: light to PIN pad |
| Volume Up / Down | yes (all three keys are one gpio-keys device; `power/wakeup` can't split them) | no light, volume only; watch Volume Up for spurious wakes (3.1) |
| RTC (S2MPG12) | after S3 | alarms and systemd timers with `WakeSystem=yes`; dark resume |
| Bluetooth host wake | yes | dark resume (a connected watch or headset); BT stays up |
| Wi-Fi | no (no `WL_HOST_WAKE` consumer; Wi-Fi unloaded in sleep until S5) | after S5: Wi-Fi stays up but can't wake; wake-on-WLAN is kernel work (`pcie-gs201` PM ops, gpa6-1 as an OOB wake IRQ) |
| USB / Type-C | no (TCPC blacklisted) | plug-in wake after USB-C-PLAN; until then the USB link simply returns on the next user wake |
| Fuel gauge alert | yes | dark resume; if S4's loop shows it waking often, disable its `power/wakeup` with a udev rule |
| AoC mailbox | yes (waking services) | stage S6 decides; sensor batching may make it fine |
| Touch | no | stays off (as stock) |

### 6.4 What to unload or quiesce before sleep

| Part | Before S5/S6 | After |
|---|---|---|
| Wi-Fi | unload in the hook (D3 untested) | keep loaded once S5 passes (`/etc/omamobile/wifi-keep-across-sleep`); the hook falls back to unloading after a D3 failure |
| AoC | keep loaded and online; **never `rmmod`** | as S6 shows; if sensor streams wake the phone, the hook stops the sensor clients (autorotate) and the proxy releases the stream |
| Sensor proxy, autorotate | frozen with userspace | as above |
| Audio | an inhibitor while playing; idle PCMs need nothing | same |
| GNSS | `omamobile-gnss` holds an inhibitor while a fix runs; otherwise the chip is off | same |
| Modem, fingerprint, NFC | not running by default | each plan adds its own pre/post when it ships |
| Haptics | no PM ops; a buzz in flight could hit a suspended I2C bus (low risk) | kernel suspend op (section 7, later) |
| Touch | keeps scanning (drain) | kernel suspend op (sleep command) |
| Bluetooth | stays up; host wake works | same |
| UFS | `spm_lvl` 5 | kernel default |

### 6.5 Wi-Fi beyond the D3 handshake (kernel, later)

- The D3 handshake (S5) keeps the link up in s2idle: the PCI core puts the
  endpoint in D3hot and `pcie-gs201` does nothing. For SYS_SLEEP, and for
  lower s2idle power, `pcie-gs201` needs PM ops: `suspend_noirq` =
  PME_Turn_Off, wait for L2, PERST#, PHY isolation (PMU `0x3ec4`), as
  Google's `exynos_pcie_rc_poweroff` (`GS/.../pcie-exynos-rc.c:3979-4116`);
  `resume_noirq` = the probe-time bring-up. A link torn down in L1.2 is
  known to make the next link training hang: exit L1 first.
- Wake-on-WLAN = request WL_HOST_WAKE (gpa6-1, FAR_ALIVE, active high) as a
  wake IRQ in brcmfmac's OOB path.
- Until S5 passes, the sleep hook unloads Wi-Fi and reloads it through
  `omamobile-wifi.service` after each wake (6.2).

## 7. Kernel work on this branch

Branch `omamobile-suspend` (`~/android/mainline/wt-suspend`), off `omamobile`
(`7b0f8b5d1`):

| Commit | What | Default |
|---|---|---|
| `8d820bc0a` soc: samsung: exynos-pmu: keep the CPU idle hint in suspend-to-idle | `exynos_cpupm_suspend_noirq()` returns early when `pm_suspend_target_state == PM_SUSPEND_TO_IDLE`, so C2 keeps its CPU_INFORM hint in s2idle | Only acts in s2idle, which nothing enters today |
| `17239293a` merge `omamobile-acpm` | ACPM link, needed by the PMIC | as in `omamobile-int` |
| `279130ea5` dt-bindings: soc: samsung: exynos-sysreg: add gs201 VGPIO2AP | `google,gs201-vgpio2ap-sysreg` | n/a |
| `a0cdfa111` dt-bindings: mfd: add Samsung S2MPG12 PMIC (and allow it under the gs201 ACPM node) | `samsung,s2mpg12-pmic` with `google,vgpio2ap-sysreg` | n/a |
| `6c0840744` mfd: sec: add S2MPG12 | ACPM regmaps (common, PM, RTC, meter) with **writes limited to the interrupt masks and the RTC**, no caching; a one-level regmap-irq on PM INT1-5 (bulk read, clear on read), whose pre-handler writes back VGPIO2AP `INTC0_IPEND` (`0x182f0290`); meter interrupts masked; IBIM1 = `0xfe` (PMIC source only), as Google's `s2mpg12_irq_init()`. Only cell: the RTC; **no regulators** | module (`CONFIG_MFD_SEC_ACPM=m`) |
| `9270c1a8c` rtc: s5m: add S2MPG12 | same RTC registers as S2MPG10 (checked against `GS/include/linux/mfd/samsung/rtc-s2mpg12.h`); alarm pending from PM STATUS2 bit 2 | built in with `RTC_DRV_S5M` |
| `9f0ff0acc` arm64: dts: exynos: google: gs201: add the VGPIO2AP sysreg | `syscon@182f0000` | node only, unused until the PMIC is enabled |
| `93555da2a` arm64: dts: exynos: google: gs201-cheetah: describe the main PMIC | `&acpm_ipc { pmic-1 }`, GIC SPI 72 level high, `wakeup-source` | **`status = "disabled"`**; overlay `suspend-pmic-test.dtso` enables it |
| `025624c64` arm64: dts: exynos: google: gs201-cheetah: default to suspend-to-idle | `mem_sleep_default=s2idle` in bootargs | on: removes the unsafe default (section 1) |
| `a7e5e1d95` scsi: ufs: exynos: power the gs201 device down in system suspend | `gs201_ufs_drv_init()` sets `hba->spm_lvl = UFS_PM_LVL_5`, then runs the shared gs101 init. The core sets its defaults before the variant init, so this wins; sysfs can still change it. gs101 is left alone (untested), though `gs101_ufs_suspend()` asserts the reset there too | on, gs201 only; acts only in system suspend (S1 vs S2 evidence) |

Notes on the PMIC work:

- **Why the RTC first:** `rtcwake` is the only unattended wake for tests, and
  later the alarm clock and background checks. With `RTC_HCTOSYS=y` it also
  sets the clock at boot.
- The RTC init writes `RTC_CTRL` = binary, 24 h (`MODEL24`), the value Google's
  driver writes. Upstream S2MPG10 code registers a WTSR restart handler only
  with `system-power-controller`, which our node doesn't have.
- `sec_pmic_suspend` arms SPI 72 for wake and disables it across sleep; the
  RTC driver arms its nested alarm IRQ. Stock does the same
  (`GS/drivers/mfd/s2mpg12-core.c:534-571`).
- External reports: reading INT2 alone left the interrupt asserted (storm),
  and the VGPIO pending bit must be cleared. regmap-irq reads INT1-5 in one
  bulk read, and the pre-handler clears the latch.

**Wi-Fi D3 (branch `omamobile-wifi-d3`, `~/android/mainline/wt-wifi-d3`,
built into `~/android/mainline/out-wifi-d3`):** `812288601` wifi: brcmfmac:
pcie: exchange mailbox data as control messages, **rebased onto
`omamobile-int` `60d3ca176`** (it was on `omamobile-w4` `242602382`, which int
contains; int has no brcmfmac changes since, so the commit applied
unchanged; the old commit was `73a5c5ccf`). For shared version >= 6 without
`USE_MAILBOX`, H2D mailbox data goes out as `MSGBUF_TYPE_H2D_MAILBOX_DATA`
(40-byte item, the control submit ring's size) and D2H
`MSGBUF_TYPE_D2H_MAILBOX_DATA` (20 bytes in a 24-byte item) is handled; no
D0 inform on resume (the host-ready doorbell, `0x12345678` on H2D mailbox 1
for BCM4389, resumes the device, as Broadcom's driver does from version 6).
Finished in this round:

- the D3 ack is now published at the end of `brcmf_pcie_rx_process()`,
  under `rx_lock`, after the control completion ring's read pointer was
  written back. That covers the interrupt thread and the poll worker alike,
  and serialises the flag (it was a plain bool touched from both);
- `brcmf_pcie_pm_enter_D3()` fails at once if the D3 inform can't be queued
  ("Failed to send D3 inform", e.g. a full control ring) instead of waiting
  2 s for an ack that can't come; the TCM path's -EIO (mailbox busy for 1 s)
  takes the same exit;
- after a successful D3 entry the poll worker is cancelled; it would only
  have seen STATE_DOWN.
- Builds with no warnings (`W=1` for `pcie.o` and `msgbuf.o`); checkpatch
  clean apart from the missing Signed-off-by (the human signs off).

The Wi-Fi start script and unit need no change for suspend: the unit is a
oneshot with no stop path, so the sleep hook (6.2) unloads the modules
itself and reloads through `systemctl restart omamobile-wifi`, which runs
the hang-safe script with its marker again. The script refuses to run after
a bring-up that hung, so a resume that wedges the chip can't loop.

Not done here, for later branches: per-bank EINT offsets for gs201 (deep
only); `pcie-gs201` PM ops; Synaptics and CS40L26 suspend; S2MPG12/13
regulators; the SYS_SLEEP driver.

Config: `kernel/configs/omamobile.config` should gain `CONFIG_PM_DEBUG=y`,
`CONFIG_PM_ADVANCED_DEBUG=y` and `CONFIG_PM_SLEEP_DEBUG=y` (`/sys/power/pm_test`,
`pm_print_times`, `pm_debug_messages`). The out-suspend build adds them from
`out-suspend/suspend-debug.config` (section "Build result"), because the
shared fragment had uncommitted changes from other work.

## 8. Staged runbook (on the phone, run by the main session)

Rules: one change at a time; slot B only; keep the previous images to flash
back; `sync` before every sleep; log to UFS, not the USB link. Each real
sleep drops the USB gadget, so commands run detached with `systemd-run` and
are read back after reconnecting.

Recovery for every stage:

- **Phone doesn't wake** (black or frozen screen, no reaction to Volume Up or
  Power for 60 s): hold Power about 30 s (PMIC long-press reset). The next
  boot is our slot B; after repeated failures ABL falls back to stock slot A
  (NOTES, "Slot B fallback works"), then `adb reboot bootloader` and flash
  the previous images.
- **Phone wakes but USB doesn't come back**: the screen should show the
  session again. Reboot from the power menu (long press). Then read the log.
- **Phone wakes with UFS errors** (`ufshcd` error handler, I/O errors,
  read-only remount): reboot at once, run `fsck` on the next boot if the
  journal says so, stop and report. Don't sleep again on that kernel.
- **Suspend refuses** (`echo` returns an error, phone never sleeps): not
  dangerous. Read `dmesg` for the device that failed and stop.

Stop conditions: any UFS error; any RCU stall, soft lockup or "SMC failed";
a wake that needs the 30 s reset; an IRQ storm on the PMIC interrupt (S3);
`state1/rejected` close to `state1/usage` across a freeze (S4).

### S0 results (2026-10-12, `omamobile-int` 00186-ga0b0c0ebb3f6)

- `state` "freeze mem disk"; `mem_sleep` "s2idle [deep]": the firmware's
  SYSTEM_SUSPEND is advertised, as predicted (section 1). No `pm_test`, no
  RTC.
- No cpuidle at all, broadcast `bc_hrtimer`: C2 and the MCT were only on
  `omamobile-power-on`, since merged into int (`741b6ed1d`). From now on
  every s2idle test runs with C2, so the exynos-pmu fix (`8d820bc0a`) must
  be in before S2.
- UFS `spm_lvl` 3, `rpm_lvl` 3, `runtime_status` active.
- Wake-enabled: BT serdev, `max170xx_battery`, the Wi-Fi function
  `0000:01:00.0`, gpio-keys. `wakeup_count` 1.
- Loaded: brcmfmac (+wcc), pcie_gs201, hci_uart, btbcm, panthor,
  cs40l26_vibra, max17042_battery.

### S0: read-only state (current kernel, no flash)

```sh
tools/phone-ssh 'uname -r; cat /proc/cmdline
cat /sys/power/state /sys/power/mem_sleep          # expect "freeze mem" and "s2idle [deep]"
ls /sys/power/pm_test 2>&1                         # expect: No such file (no PM_DEBUG yet)
ls /sys/class/rtc/                                 # expect: empty (no RTC yet)
ls /sys/devices/system/cpu/cpu0/cpuidle 2>&1 | head -3
cat /sys/devices/system/clockevents/broadcast/current_device
U=$(ls -d /sys/bus/platform/devices/*.ufs); echo $U
cat $U/spm_lvl $U/rpm_lvl $U/power/runtime_status
for d in /sys/bus/scsi/devices/*:49488; do echo "$d $(cat $d/power/runtime_status) $(cat $d/power/control)"; done
grep -E "gpio-keys|max17042|host-wake|bcm|ufs|dwc3" /proc/interrupts
grep -l enabled $(find /sys/devices -path "*power/wakeup" 2>/dev/null) 2>/dev/null
cat /sys/power/wakeup_count
cat /sys/kernel/debug/wakeup_sources 2>/dev/null | awk "NR==1 || \$2>0" | head -30
systemd-analyze cat-config systemd/sleep.conf | grep -v "^#" | grep .
lsmod | grep -E "brcmfmac|pcie_gs201|panthor|cs40l26|hci_uart|btbcm|max17042"
cat /sys/class/power_supply/max170xx_battery/{status,current_now}'
```

As the logged-in user on the phone: `omarchy-toggle-enabled suspend-off && echo
"suspend hidden"`.

Record in NOTES. What decides the next stages:

- `mem_sleep` showing `deep` confirms section 1 (and that "mem" is unsafe on
  this kernel).
- If the device WLUN (`:49488`) is `suspended` or `control` is `auto`, UFS
  runtime suspend already runs the reset path daily (good evidence for S2).
- The list of enabled wakeup devices; any gauge interrupt counting fast.

### S1: new kernel, dry runs with pm_test (flash)

Merge order into `omamobile-int` (each merges cleanly on `741b6ed1d`, checked
with `git merge-tree`):

1. `omamobile-suspend` (`025624c64`): s2idle default, exynos-pmu hint fix,
   S2MPG12 + RTC (node disabled).
2. `omamobile-chgguard` (`37a83337c`, on `omamobile-usbc`, already in int):
   the charger temperature guard, off by default (USB-C-PLAN B3g).
3. Not yet: `omamobile-wifi-d3` (now `812288601`, rebased onto int and
   built). Merge for S5.

`kernel/configs/omamobile.config` has the PM debug options since `38358ac`.
Build and flash slot B as usual. This
kernel changes nothing at runtime except `mem_sleep` (now `[s2idle] deep`)
and the new `/sys/power/pm_test`. The PMIC node is still disabled.

```sh
tools/phone-ssh 'cat /sys/power/mem_sleep /sys/power/pm_test   # expect "[s2idle] deep", "[none] core processors platform devices freezer"
systemctl stop omamobile-wifi; modprobe -r brcmfmac pcie_gs201; lsmod | grep -c brcmfmac'
```

Then three dry runs, each 5 s and automatic (no wake source needed), freezer
first. They keep USB down for a few seconds, so run detached:

```sh
tools/phone-ssh 'mkdir -p /var/lib/omamobile/suspend; systemd-run --unit=s1-test sh -c "
  L=/var/lib/omamobile/suspend/s1-\$(date +%H%M%S).log
  echo 1 > /sys/power/pm_print_times; echo 1 > /sys/power/pm_debug_messages
  for t in freezer devices platform; do
    sync; echo \$t > /sys/power/pm_test
    echo == \$t >> \$L; date >> \$L
    echo freeze > /sys/power/state; echo rc=\$? >> \$L
    sleep 10
  done
  echo none > /sys/power/pm_test; dmesg | tail -300 >> \$L; sync"'
# wait about a minute, reconnect
tools/phone-ssh 'tail -5 /var/lib/omamobile/suspend/s1-*.log; dmesg | grep -iE "PM:|suspend|resume|ufshcd|error|fail" | tail -60'
```

**Pass:** three `rc=0`; "PM: suspend debug: Waiting for 5 second(s)" at each
level; no device "failed to suspend"; no `ufshcd` errors; Hyprland and
the touch screen work afterwards; the GPU renders (Panthor resumed). Note the
slowest devices from `pm_print_times`. **Fail:** read which device returned
an error, stop and report.

**S1 result (2026-10-12, 7.3.0-rc6-omamobile-push1-00283-gafa4d777403d,
int with `omamobile-suspend`, `omamobile-chgguard` and `omamobile-modem`
merged): passed, with one UFS finding.** `mem_sleep` `[s2idle] deep`,
`pm_test` present. Wi-Fi unloaded. `freezer`, `devices` and `platform`
each returned `rc=0`; the phone came back on USB about a minute later and
the session was still running. The log keeps only the last level
(`platform`):
- UFS failed its first hibern8 exit on resume (`ufshcd_uic_hibern8_exit:
  hibern8 exit failed. ret = 5`, `__ufshcd_wl_resume`), then a host reset
  restored the link (`Power mode changed to : FAST series_B G_4 L_2`), and
  the device WLUN resumed with 0 after 250 ms. No I/O or filesystem
  errors. This is what S2's `spm_lvl` question is about.
- Slowest resumes: the UFS device WLUN 250 ms (the reset), dwc3 113 ms.
  Device resume took 252 ms in total.

### S2: first real freeze, woken by Volume Up (attended)

```sh
tools/phone-ssh 'U=$(ls -d /sys/bus/platform/devices/*.ufs); echo 5 > $U/spm_lvl
systemd-run --unit=s2-test sh -c "L=/var/lib/omamobile/suspend/s2-\$(date +%H%M%S).log
  date >> \$L; sync; echo freeze > /sys/power/state; echo rc=\$? >> \$L; date >> \$L
  cat /sys/power/pm_wakeup_irq >> \$L 2>&1; dmesg | tail -200 >> \$L; sync"'
# after about 20 s press Volume Up once; the screen should respond; reconnect USB
```

**Pass:** the phone resumes on Volume Up; `pm_wakeup_irq` is the gpio-keys
interrupt; no UFS errors; touch, display and GPU work. Then repeat with
`spm_lvl` 3 once, to learn whether the default level survives (if it does
not, a gs201 `spm_lvl` 5 default becomes a kernel commit).

**S2 result (2026-10-12, 7.3.0-rc6-omamobile-push1-00287-g3ff963085301):
passed on the second try.** Wi-Fi unloaded, UFS `spm_lvl` 3 -> 5.
- **First run:** `rc=0`, but it resumed about 3 s after suspend entry.
  `pm_wakeup_irq` was 106, the gpio-keys `KEY_VOLUMEUP` line (gpa8-5), and
  the human reports not touching the button. Volume Up is the one key with
  no internal pull (`PUD` 0x0, as stock; Volume Down and Power have 0x3),
  so a false edge as the pins enter their power-down state is possible. It
  is not proven: two edges were counted on that line, as one press makes.
  Watch for spurious wakes; if they recur, check gpa8-5's `PUD_PDN` and
  try a pull-up in the DT.
- **Second run:** asleep 39 s (wall clock), woken by Volume Up (IRQ 106) as
  intended. **No UFS error** on resume at `spm_lvl` 5 (S1 at 3 needed a
  link reset). Bluetooth logged "Opcode 0x0c24 failed: -112" on suspend
  entry (harmless here; the controller stays up).
- The `spm_lvl` 5 result argues for making 5 the gs201 default (a kernel
  commit), pending more cycles.

### Common setup for S3 onwards

Every stage below runs on `omamobile-int` with the branches it names merged,
built with an explicit tree and output directory, and with the DTB overlays
the phone runs now. Helpers, on the laptop:

```sh
cd ~/Projects/omarchy-mobile
export KERNEL_DIR=$HOME/android/mainline/wt-int KERNEL_OUT=$HOME/android/mainline/out-int
O=$KERNEL_OUT D=$O/arch/arm64/boot/dts/exynos/google/gs201-cheetah.dtb
# Freeze test, detached, logged to UFS (USB drops while asleep). Usage:
#   tools/phone-ssh "$(freeze_cmd NAME [rtcwake-seconds])"
freeze_cmd() {
  local n=$1 secs=${2:-}
  local enter='echo freeze > /sys/power/state'
  [[ -n $secs ]] && enter="rtcwake -m freeze -s $secs"
  printf '%s' "mkdir -p /var/lib/omamobile/suspend; systemd-run --collect --unit=$n-test sh -c '
    L=/var/lib/omamobile/suspend/$n-\$(date +%H%M%S).log
    U=\$(ls -d /sys/bus/platform/devices/*.ufs); echo spm_lvl=\$(cat \$U/spm_lvl) >> \$L
    cat /sys/power/wakeup_count /sys/power/suspend_stats/success /sys/power/suspend_stats/fail >> \$L
    date >> \$L; sync; $enter; echo rc=\$? >> \$L; date >> \$L
    echo wakeup_irq=\$(cat /sys/power/pm_wakeup_irq 2>&1) >> \$L
    grep -E \"^ *\$(cat /sys/power/pm_wakeup_irq 2>/dev/null):\" /proc/interrupts >> \$L
    dmesg | tail -200 >> \$L; sync'"
}
```

Before every freeze (until the stage says otherwise): Wi-Fi unloaded,
`tools/phone-ssh 'modprobe -r brcmfmac_wcc brcmfmac pcie_gs201; lsmod | grep -c brcm'`
(expect 0). After every freeze: `dmesg | grep -E "ufshcd_err_handler|hibern8|I/O error"`
must be empty.

### S3: the UFS default and the RTC (two boots)

**S3a: kernel with the UFS default (flash, low risk).** One change: `spm_lvl`
5 by default. Nothing else changes at runtime (it only acts in system
suspend, as S2's runtime `echo 5` did).

```sh
cd ~/android/mainline/wt-int && git merge --no-ff omamobile-suspend   # a7e5e1d95
cd ~/Projects/omarchy-mobile && bin/omamobile kernel > logs/kernel-s3.log 2>&1 && bin/omamobile modules
# make dtbs may have rewritten $D from source: if $D is newer than $D.default, the
# build replaced the overlaid DTB; refresh the base and re-apply the phone's overlays
# with the ov helper (AUDIO-PLAN.md) before images. Otherwise $D is still overlaid.
bin/omamobile images > logs/images-s3a.log 2>&1
# ask the human, then: bin/omamobile push   (reboots)
tools/phone-ssh 'uname -r; U=$(ls -d /sys/bus/platform/devices/*.ufs); cat $U/spm_lvl $U/rpm_lvl'
```

- **Expect/Pass:** boots as before (USB, SSH, touch, display, GPU, audio);
  `spm_lvl` 5, `rpm_lvl` 3. Then one Volume Up freeze with Wi-Fi unloaded
  (`tools/phone-ssh "$(freeze_cmd s3a)"`, press Volume Up after about 20 s):
  `rc=0`, no UFS error, wake IRQ = gpio-keys.
- **Stop:** any UFS error, or `spm_lvl` not 5.
- **Rollback:** push the previous images (kept in `out-int/images` of the
  last build: copy them aside before building), or `echo 3 > spm_lvl` at
  runtime.
- **Risk:** low (same level S2 used at runtime).

**S3b: the PMIC overlay (flash, medium risk: ACPM traffic and a new IRQ).**
The phone's DTB already carries test overlays, so apply this one on top of
the current DTB and keep that DTB for rollback:

```sh
cp "$D" "$D.pre-s3"
$O/scripts/dtc/dtc -q -I dts -O dtb -o out/suspend-pmic-test.dtbo device/cheetah/overlays/suspend-pmic-test.dtso
$O/scripts/dtc/fdtoverlay -i "$D.pre-s3" -o "$D" out/suspend-pmic-test.dtbo
diff <($O/scripts/dtc/dtc -q -I dtb -O dts "$D.pre-s3") <($O/scripts/dtc/dtc -q -I dtb -O dts "$D")
#   expect exactly one change: pmic-1 gains status = "okay"
bin/omamobile images > logs/images-s3b.log 2>&1
# ask the human, then: bin/omamobile push
tools/phone-ssh 'dmesg | grep -iE "s2mpg12|sec-pmic|sec_pmic|rtc|acpm" | tail -30; ls /sys/class/rtc
grep -E "s2mpg12|sec-pmic|rtc" /proc/interrupts; sleep 10; grep -E "s2mpg12|sec-pmic|rtc" /proc/interrupts
hwclock -r; date -u'
```

- **Expect:** `sec_acpm` (module) probes the S2MPG12, `rtc0` registered,
  HCTOSYS "setting system clock" (the RTC may be off by hours if stock never
  synced it; `hwclock -w` after NTP fixes that, a write the RTC driver is
  allowed); the PMIC interrupt (GIC SPI 72) count **not growing** while idle.
- **Stop:** a growing SPI 72 count (storm), any ACPM timeout or "SMC failed",
  or a probe error. Then `modprobe -r sec_acpm` if it unloads, and roll back.
- **Rollback:** `cp "$D.pre-s3" "$D"; bin/omamobile images` and push.
- **Risk:** medium.

Then an alarm while awake, then the first unattended freeze:

```sh
tools/phone-ssh 'rtcwake -m no -s 15; sleep 20; grep -E "s2mpg12|sec-pmic|rtc" /proc/interrupts; cat /sys/class/rtc/rtc0/wakealarm'
tools/phone-ssh 'modprobe -r brcmfmac_wcc brcmfmac pcie_gs201'
tools/phone-ssh "$(freeze_cmd s3b 30)"
# do not touch the phone; reconnect after about 60 s
tools/phone-ssh 'tail -n +1 /var/lib/omamobile/suspend/s3b-*.log | grep -E "spm_lvl|rc=|wakeup_irq|s2mpg12|sec-pmic|rtc|ufshcd|error" | tail -20'
```

- **Pass:** the awake alarm raises exactly one PMIC interrupt and the
  `wakealarm` clears; the freeze returns on its own after about 30 s with
  `wakeup_irq` = the PMIC interrupt (SPI 72), `rc=0`, no UFS error, and USB
  comes back by itself. From here on `rtcwake -m freeze` is the unattended test.
- **Stop:** no wake after 60 s (hold Power about 30 s, roll back), a wake on
  another IRQ every time, an interrupt storm after resume.
- **Rollback:** as S3b.

**S3b result (2026-10-12, 7.3.0-rc6-omamobile-push1-00345-g606b84c7a9fd,
overlays incl. `suspend-pmic-test`, `camera-pwr-test`, `modem-cpif-test`;
run unattended): passed.** pmic-1 enabled; `sec_acpm` is blacklisted
(`/etc/modprobe.d/omamobile-g1.conf`, the GNSS stack loads it on demand), so
it was loaded by hand: "s5m-rtc s2mpg12-rtc: registered as rtc0", "setting
system clock to 2026-10-13T00:08:09 UTC", and the RTC agreed with NTP time
to the second (no `hwclock -w` needed). `s2mpg12-pmic` (128, level) and
`rtc-alarm0` (129, edge) stayed at 0 for 10 s idle (no storm). The awake
`rtcwake -m no -s 15` raised rtc-alarm0 once (pmic 2) and `wakealarm`
cleared. Freeze with Wi-Fi unloaded: `rtcwake -m freeze -s 30` slept
31 s (wall clock) and woke by itself (rtc-alarm0 +1), rc 0, no UFS error,
USB back on its own, Wi-Fi restarted. The AoC and the sensor proxy ran
through it. To make the RTC part of every boot, `sec_acpm` must load at boot
(drop it from the G1 blacklist once GNSS's own loading tolerates it being
loaded already).

### S4: C2 in s2idle, then a loop (no flash)

Kernel and DTB from S3b. C2 and the MCT are in the default DTB since
`omamobile-power-on` was merged into int.

```sh
tools/phone-ssh 'for c in /sys/devices/system/cpu/cpu[0-9]*/cpuidle/state1; do
  echo "$c $(cat $c/usage $c/rejected $c/s2idle/usage $c/s2idle/time)"; done > /var/lib/omamobile/suspend/s4-before.txt
modprobe -r brcmfmac_wcc brcmfmac pcie_gs201'
tools/phone-ssh "$(freeze_cmd s4 60)"
# reconnect after about 90 s
tools/phone-ssh 'for c in /sys/devices/system/cpu/cpu[0-9]*/cpuidle/state1; do
  echo "$c $(cat $c/usage $c/rejected $c/s2idle/usage $c/s2idle/time)"; done | paste /var/lib/omamobile/suspend/s4-before.txt -'
```

- **Pass:** `s2idle/usage` grows on every CPU; `rejected` grows by a small
  fraction of `usage` (as awake, POWER-PLAN 7); about 60 s of `s2idle/time`
  (us) per CPU.
- **Stop:** `rejected` close to `usage` (the exynos-pmu hint fix isn't
  acting), an RCU stall, a wake that needs the 30 s reset.
- **Rollback:** none needed; to rule C2 out, `echo 1 > .../state1/disable`
  on every CPU and repeat.

Then 20 cycles of 30 s asleep / 15 s awake, detached, counting wakes by IRQ:

```sh
tools/phone-ssh 'modprobe -r brcmfmac_wcc brcmfmac pcie_gs201; systemd-run --unit=s4-loop sh -c "
  L=/var/lib/omamobile/suspend/s4-loop-\$(date +%H%M%S).log
  for i in \$(seq 20); do sync; rtcwake -m freeze -s 30 >/dev/null 2>&1; rc=\$?
    echo \"\$i rc=\$rc irq=\$(cat /sys/power/pm_wakeup_irq 2>/dev/null) \$(date +%T)\" >> \$L
    dmesg | grep -qE \"ufshcd_err_handler|hibern8 exit failed|I/O error\" && { echo UFS-ERROR >> \$L; break; }
    [ \$rc = 0 ] || break; sleep 15; done; sync"'
# about 15 minutes; then
tools/phone-ssh 'cat /var/lib/omamobile/suspend/s4-loop-*.log; cat /sys/power/suspend_stats/fail'
```

- **Pass:** 20 lines, all `rc=0`, `irq` = the PMIC interrupt every time (a
  gpio-keys or gauge IRQ with nobody touching the phone is a spurious wake:
  record which, see 3.1 and 6.3), no UFS error.
- **Stop:** any failed cycle stops the loop by itself; read why.
- **Risk:** low once S3b passed.


**S4 loop result (2026-10-12, same boot as S3b, unattended): passed.** 20
cycles of `rtcwake -m freeze -s 20` with Wi-Fi unloaded and the AoC, sensor
proxy and speaker protection running: all `rc=0`, every wake on
`pm_wakeup_irq` 128 (s2mpg12-pmic, the RTC alarm's parent). 18 slept 21 s;
cycles 8 and 12 woke after 13 s, also on the PMIC interrupt (some other
PMIC event: worth reading the PMIC's interrupt status if it recurs). UFS
error handler count 0 throughout, no I/O or ext4 errors, no key-interrupt
wakes (the Volume Up question saw no spurious wake in 20 cycles). Wi-Fi,
AoC and the speaker services fine afterwards. The C2 counter part of S4
was not run.

### S5: Wi-Fi across sleep (flash: `omamobile-wifi-d3`)

`omamobile-wifi-d3` (`812288601`) sits on top of `omamobile-int` `60d3ca176`.
Merge it into int (a fast-forward if int hasn't moved) and build as in S3a,
keeping the S3b DTB:

```sh
cd ~/android/mainline/wt-int && git merge omamobile-wifi-d3
cd ~/Projects/omarchy-mobile && bin/omamobile kernel > logs/kernel-s5.log 2>&1 && bin/omamobile modules && bin/omamobile images
# ask the human, then: bin/omamobile push
```

**S5a, awake regression (read-only):** Wi-Fi at boot as before.

```sh
tools/phone-ssh 'dmesg | grep -iE "brcmf.*(error|fail|timeout)|Unsupported msgtype" | tail -20
W=$(ls /sys/class/net | grep -m1 "^wl"); nmcli -t -f DEVICE,STATE dev | grep "^$W"
ping -c 5 -I $W $(ip -4 route show default dev $W | awk "{print \$3}")'
```

- **Expect/Pass:** connected, ping works, no new errors, no "Unsupported
  msgtype" over 10 minutes of use (the new code only acts on mailbox
  messages, which the firmware sends around D3; brcmfmac never opts in to
  inband device wake, so no deep-sleep requests are expected awake).
- **Stop:** Wi-Fi doesn't come up or drops (roll back: previous images).

**S5b, one freeze with Wi-Fi loaded and connected (attended, medium risk):**

```sh
# PCIE debug messages (CONFIG_BRCMDBG=y): D3 inform/ack and resume. Chatty, so only for this test.
tools/phone-ssh 'echo 0x80000 > /sys/module/brcmfmac/parameters/debug'
tools/phone-ssh "$(freeze_cmd s5b 30)"
# reconnect after about 60 s
tools/phone-ssh 'echo 0 > /sys/module/brcmfmac/parameters/debug
grep -E "rc=|wakeup_irq" /var/lib/omamobile/suspend/s5b-*.log
dmesg | grep -E "D3|D2H_MB_DATA|Hot resume|probe after resume|commonring|Timeout on response" | tail -30
nmcli -t -f DEVICE,STATE dev | grep "^wl"'
```

- **Pass:** `rc=0`; no "Timeout on response for entering D3 substate", no
  "Failed to send D3 inform"; "D2H_MB_DATA: D3 ACK" in the debug output; after resume
  "Hot resume" (not a re-probe), and Wi-Fi reconnected within about 10 s;
  `ping` works.
- **Stop:** `rc` non-zero with `last_failed_dev` = `0000:01:00.0` (the D3
  ack never came: record the debug output and stop); "Failed to reserve space
  in commonring" (firmware wedged: reboot, record); no wake.
- **Rollback:** unload Wi-Fi before sleeping again; previous images.

**S5a result (2026-10-12, 7.3.0-rc6-omamobile-push1-00363-g7987ffb1fd0b, int
`7987ffb1f` = int + `omamobile-wifi-d3` `812288601`): passed.** Wi-Fi
connected at boot, ping 0% loss, only the known `ROAM_TRIGGER -52` and
p2p-dev -5 lines.

**S5b result (same kernel, `debug=0x80000`, `rtcwake -m freeze -s 30` with
Wi-Fi connected): failed, and Wi-Fi stayed dead until a reboot.**
(`logs/s5b-journal-full.txt` on the laptop.)

- `brcmf_pcie_pm_enter_D3` sent the D3 inform as a control message (H2D
  control ring host w 19 → 20, doorbell rung). Within about a millisecond
  the dongle **stopped its H2D and D2H receive DMA engines** (`ctrl
  0x05200801` → `0x00000000`) and rang D2H doorbell 0 (`mailboxint` 0x1),
  but its control completion ring never moved (D2H w 3, in host memory and
  in TCM). No D3 ACK reached the driver. "Timeout on response for entering
  D3 substate", `pci_pm_suspend` -5, suspend aborted.
- After that the dongle's own function interrupt status (DAR `0xA10`) read
  `0x100` = `PCIE_INT_MB_FN0_0`, a host-to-dongle mailbox interrupt it never
  serviced, and the H2D control write index climbed (50, 51, ...) while the
  dongle's read index stayed at 19: every command timed out. The dongle was
  in D3 and nothing brought it back.
- **Why no ACK.** Broadcom's driver (Google's GPL bcmdhd for this chip,
  `dhd_pcie.c`, `dhdpcie_bus_process_mailbox_intr()`) reads the
  shared-memory D2H mailbox (`dhdpcie_read_handle_mb_data()`, for shared
  version >= 6) whenever FN0_0/FN0_1 (0x100/0x200) is set, also on core rev
  >= 64 where those bits double as D2H4 doorbells, and in its no-interrupt
  D3 path polls that mailbox every millisecond. brcmfmac's rev >= 64
  register sets had `int_fn0 = 0`: it never looked there. The control-ring
  ACK path it does have saw nothing.
- **The "12 ms" timeout was not 12 ms.** printk time on this kernel is the
  MCT FRC (`sched_clock: 32 bits at 25MHz`, registered after the arch timer,
  and preferred for its higher rate), while jiffies and ktime run on the arch
  counter. In the suspend window the two disagree: the freezer measured 3 ms
  and 1 ms (ktime) where printk shows 0.2 ms and 0.06 ms, and the poll
  worker's 50 jiffies-timed passes fit in "10 ms" of printk time. The wall
  clock agreed with the RTC across S3b (31 s), so ktime is right and **printk
  timestamps around suspend are not**. The 2 s wait most likely ran in full.
  The MCT as `sched_clock` on arm64 is an upstream matter (`exynos_mct.c`
  registers it unconditionally); for now, read durations from the driver's
  own ktime-based numbers, not from dmesg timestamps.
- **Fixed on `omamobile-wifi-d3`, `e20614037`** (on top of `812288601`;
  builds, W=1 and checkpatch clean apart from the sign-off):
  - rev >= 64 register sets read the shared-memory mailbox on FN0 (under
    `rx_lock`);
  - the D3 wait polls that mailbox and the control completion ring every
    2 ms besides the wake-up, for up to 2 s (jiffies);
  - after the ACK, device interrupts are masked and cleared until resume,
    as Broadcom's driver does;
  - ~~a missed ACK requests a bus reset~~: withdrawn in `f8c94c615` after
    S5b-2 step 2 (below). A missed ACK now logs "device unresponsive after
    D3 inform; reload the driver stack to recover" and leaves recovery to a
    full-stack reload, which the sleep hook does;
  - a failed send restarts the firmware console timer (it was left off);
  - hot resume reads the mailbox and the rings once and restarts the poll;
  - one `dev_dbg()` line per D3 entry ("D3 ACK by control message|shared-memory
    mailbox after N us"), per hot resume ("hot resume") and per lost state
    ("probing again"). `CONFIG_BRCMDBG` builds define `DEBUG`, so these print
    with **no `debug` flag at all**; the error line carries the measured
    wait in ms and `mailboxint`.

**Quieter debugging.** `debug=0x80000` also enables the bring-up ring dumps
(every doorbell and index update), which overflowed the kernel log during
S5b. For S5b-2 leave `debug` at 0: the new `dev_dbg` lines and the error
lines are enough. If more is needed, `debug=0x40000` (MSGBUF: one line per
message, including `MSGBUF_TYPE_D2H_MAILBOX_DATA`) with the link idle, set
just before the freeze and cleared right after. Never 0x80000 across a
suspend.

**S5b-2 first run (2026-10-12, 7.3.0-rc6-omamobile-push1-00367-g054dfb8de239,
int `054dfb8de` with `e20614037`).** Step 1 passed (`wlp1s0` connected, 5/5
ping, only the old boot lines). **Step 2, the debugfs bus reset, oopsed and
wedged the phone** (manual power cycle):

```
ieee80211 phy0: brcmf_fil_cmd_data: bus is down. we have nothing to do.
Workqueue: events brcmf_pcie_poll_worker [brcmfmac]
pc : brcmf_proto_msgbuf_rx_trigger+0x24/0x144 [brcmfmac]
lr : brcmf_pcie_poll_worker+0x94/0xfc [brcmfmac]
brcmfmac: brcmf_fw_alloc_request: using brcm/brcmfmac4389c1-pcie for chip BCM4389/3
```

then SSH sessions opened but no command ran (a lock held by the dead
worker).

- **Cause:** `brcmf_pcie_reset()` only masked the device's interrupts before
  `brcmf_detach()` freed the protocol layer. The interrupt thread, the
  console timer and our BCM4389 D2H poll worker (from `omamobile-wifi`, not
  upstream) stayed live; the worker processed the rings through freed
  memory and died holding `rx_lock`. `brcmf_pcie_remove()` does it right
  (STATE_DOWN, cancel the poll, synchronise the interrupt); the reset path
  never did. Upstream's reset has the same gap for the interrupt thread,
  with nothing as busy as our poll to hit it.
- **Was the reset ever good on this chip? No evidence that it was.** It was
  never run before. And a firmware re-download without a power cycle has
  only been seen to fail here: WIFI-BATTERY-PLAN 9.14 W2.4, a driver-only
  reload, gave "FW failed to initialize"; the FLR fix for that has no
  recorded result. Only a reload of the whole stack (`pcie_gs201` too, which
  cycles WL_REG_ON) is proven. So the missed-ACK auto-reset from `e20614037`
  inherited both problems.
- **Fixed in `f8c94c615`** (on `omamobile-wifi-d3`; builds, W=1 and
  checkpatch clean apart from the sign-off; merges cleanly into int
  `a1a0d9a66`):
  - `brcmf_pcie_reset()` first sets STATE_DOWN, stops the console timer,
    masks and synchronises the interrupt, and cancels the poll, as remove
    does; `brcmf_pcie_setup()` brings the state back up;
  - the RX path refuses to run without a protocol layer (`rx_process` and
    `brcmf_proto_msgbuf_rx_trigger()`);
  - the missed-ACK path no longer rings host-ready (this firmware traps on an
    unexpected one, see `brcmf_pcie_preinit()`) or resets: it logs "device
    unresponsive after D3 inform; reload the driver stack to recover";
  - the sleep hook (`omamobile-sleep`, not yet released) now reloads the whole
    stack right after a suspend that failed in the Wi-Fi device, besides
    switching to unload mode for the boot.

**S5b-2, revised (flash `f8c94c615`; attended).** Merge `omamobile-wifi-d3`
into `wt-int`, build, keep the DTB, push. Before every step stream the
kernel log to the laptop, so an oops is captured whole even if the phone
wedges:

```sh
tools/phone-ssh 'dmesg -W' > logs/s5b2-kmsg-$(date +%H%M%S).log 2>&1 &
```

1. **Awake regression (low risk):** as before (Wi-Fi up, ping, no new
   brcmfmac errors after 10 minutes).
2. **The full-stack reload (low risk, the proven recovery):**
   `tools/phone-ssh 'modprobe -r brcmfmac_wcc brcmfmac pcie_gs201 && systemctl restart omamobile-wifi; sleep 20; nmcli -t -f DEVICE,STATE dev | grep "^wl"'`.
   **Pass:** connected again. **Stop:** anything else (this is what every
   failure below relies on).
3. **The debugfs bus reset again (medium risk; tests the teardown fix, not
   a recovery we depend on):**

   ```sh
   tools/phone-ssh 'echo 1 > /sys/kernel/debug/ieee80211/phy0/reset; sleep 30
   dmesg | grep -iE "brcmf|ieee80211|Oops|BUG" | tail -25; ip -br link | grep wl'
   ```

   - **Pass (teardown):** no oops, no hang; SSH keeps working.
   - **Then either:** Wi-Fi comes back (the re-download works: note it, the
     reset becomes usable for recovery later), or "FW failed to initialize"
     / no interface (expected from W2.4: recover with step 2's reload).
   - **Stop:** an oops or a hang (power cycle by the human; send the
     streamed log).
4. **One freeze with Wi-Fi connected, `debug` 0** (as step 3 of the first
   version):

   ```sh
   tools/phone-ssh 'echo 0 > /sys/module/brcmfmac/parameters/debug'
   tools/phone-ssh "$(freeze_cmd s5b2 30)"
   # reconnect after about 60 s
   tools/phone-ssh 'grep -E "rc=|wakeup_irq" /var/lib/omamobile/suspend/s5b2-*.log
   dmesg | grep -E "D3 ACK by|hot resume|probing again|D3 substate|D3 inform|unresponsive" | tail
   nmcli -t -f DEVICE,STATE dev | grep "^wl"; cat /sys/power/suspend_stats/last_failed_dev'
   ```

   - **Pass:** `rc=0`, RTC wake after about 30 s, "D3 ACK by ... after N us"
     (record the path and N), "hot resume", Wi-Fi back within about 10 s.
   - **ACK still missed:** `rc` non-zero, "Timeout on response for entering
     D3 substate (N ms, mailboxint ...)" and "device unresponsive". Record
     N and `mailboxint`, recover with step 2's reload (this freeze ran
     without the sleep hook), stop, and keep unload mode.
   - **Stop:** a hang, an oops, or Wi-Fi not back after step 2's reload.
5. Then S5c.

**S5b-2 revised, results (2026-10-13, 7.3.0-rc6-omamobile-push1-00372-g6e3b1e5164d6):**

1. Awake: passed (ping over Wi-Fi; only the usual boot-time brcmfmac lines).
2. Full-stack reload: **passed** (connected again within 20 s). The reload
   registers a new wiphy (`phy1`), so later steps must glob
   `/sys/kernel/debug/ieee80211/phy*/reset` rather than name `phy0`.
3. debugfs bus reset: **passed.** No oops and no hang (the teardown fix
   works). The firmware downloaded again and Wi-Fi reconnected by itself, so
   the reset is usable as a recovery.
4. Freeze with Wi-Fi connected: **ACK still missed.** `rc=1` and "Timeout
   on response for entering D3 substate (1999 ms, mailboxint 0x00000000)",
   then "device unresponsive after D3 inform"; `last_failed_dev` was
   0000:01:00.0. Nothing reached the shared-memory mailbox either. Step 2's
   reload recovered Wi-Fi. The sleep hook stays in unload mode.

**S5b-3 (2026-10-13, 7.3.0-rc6-omamobile-push1-00375-gc73a652bd07a): passed.**
The wake-up ordering was already right (`812288601` publishes the ACK only
after the ring's read pointer is written back). The real cause: the D3
inform, an H2D mailbox-data message, went out without the epoch (sequence)
byte that `9f7b5cac4` added to every other control-ring message. Firmware
20.101.90 checks it, so it never consumed the inform: the dongle's read
index stopped on that slot, and every later control message queued behind
it. Google's GPL bcmdhd numbers mailbox data like any other control
message. Fix: `3dc1f0098` (number the mailbox-data message), plus
`1505e2a20` (`synchronize_irq()` before masking for D3, so the interrupt
thread that took the ACK can't unmask again). One freeze with Wi-Fi
connected (`freeze_cmd s5b3 30`): `rc=0`, RTC wake, "D3 ACK by control
message after 13864 us", "hot resume", Wi-Fi connected, and ping working
right after. Next: S5c.

**S5c, 10 then 20 cycles with Wi-Fi loaded:** S4's loop without the
`modprobe -r`, adding `nmcli -t -f STATE general` to each log line.

- **Pass:** every cycle `rc=0` and Wi-Fi back each time (external reports saw
  about 2 failures in 20 cycles before the ack-ordering fix, 0 in 8 with it).
- **Stop:** the first D3 timeout or commonring error. A rare one means the
  sleep hook stays in unload mode by default (its fallback covers it); a
  frequent one means more driver work.
- **After passing:** `touch /etc/omamobile/wifi-keep-across-sleep` (the hook
  then keeps Wi-Fi loaded).

### S6: the AoC and sensors across sleep (no flash)

Kernel from S5 (or S3b); Wi-Fi unloaded to keep one variable. The AoC
starts at boot (`omamobile-aoc.service`), the sensor proxy with it.

```sh
tools/phone-ssh 'systemctl is-active omamobile-aoc omamobile-sensor-proxy
grep -iE "mbox|aoc" /proc/interrupts; dmesg | grep -iE "aoc.*(offline|crash|watchdog)" | tail -5'
```

As the logged-in user on the phone: `systemctl --user is-active omamobile-autorotate`.

1. **S6a, sensors idle** (autorotate stopped by the user from the session:
   `systemctl --user stop omamobile-autorotate`), one `freeze_cmd s6a 60`.
2. **S6b, autorotate running** (the accelerometer stream claimed), one
   `freeze_cmd s6b 60`.
3. After each: the AoC still online (no "AoC offline", no ramdump, no
   watchdog in dmesg); `tools/aoc-audio-diag` with `EP=6` plays its tone;
   `monitor-sensor` (iio-sensor-proxy client) shows accelerometer and light.

- **Pass:** both freezes return after 60 s on the RTC, not earlier; the AoC,
  audio and all four sensors work after resume. Record the AoC mailbox IRQ
  count before and after each freeze.
- **Interpretation:** if S6b wakes early on the AoC mailbox IRQ (or its
  count climbs during the freeze), sensor streams wake the phone: the sleep
  hook then stops `omamobile-autorotate`'s stream before sleep (6.4). If S6a
  already wakes early, find the waking service from `aoc_channel_dev`'s
  wakelock (`/sys/kernel/debug/wakeup_sources`).
- **Stop:** the AoC goes offline or crashes across sleep (don't restart it by
  hand: reboot, AGENTS.md), or audio stops working after resume.
- **Risk:** medium (first sleep with the AoC up; S2-S4 ran with it up too, so
  partly covered: compare their logs first).

### S7: battery numbers

Cable out, as POWER-PLAN section 4 (`power-ab`): A = screen off and awake,
B = screen off in s2idle (one long `rtcwake -m freeze` per phase). Wi-Fi and
BT as they would be in use. This gives the honest s2idle number to set
against SYS_SLEEP.

### S8: userspace on (package release)

1. New `omamobile-device-cheetah` release with the files already written
   (`sleep.conf.d/10-omamobile.conf`, `bin/omamobile-sleep` and its
   `system-sleep` link), version bumped. Install with
   `tools/phone-install-package packages/omamobile-device-cheetah`.
   Check: `systemd-analyze cat-config systemd/sleep.conf | grep MemorySleepMode`
   = s2idle; `ls -l /usr/lib/systemd/system-sleep/omamobile`.
2. With `suspend-off` still set, from SSH: `systemctl suspend`, wake with
   Volume Up after 20 s (or with the RTC: `rtcwake -m no -s 30` first).
   **Pass:** the journal shows `omamobile-sleep: Wi-Fi unloaded for sleep`,
   `woken by IRQ ...`, `Wi-Fi reload queued`, and Wi-Fi reconnects.
   **Stop:** the hook fails or Wi-Fi doesn't return (then the hook is wrong,
   not the kernel).
3. `omamobile-sleepd` and the `omamobile-keys` change (6.2) behind the
   `omamobile-sleep` toggle; a day of normal use: power key → sleep → power
   key, idle → sleep, an RTC alarm, a BT device connected.
4. Clear `suspend-off` in the release after that day.

- **Rollback:** `omarchy-toggle-disable omamobile-sleep`; set `suspend-off`
  again; previous package from the repository.

## 9. Risks and rollback

| Change | Risk | Default | Rollback |
|---|---|---|---|
| `mem_sleep_default=s2idle` | none known; "mem" stops meaning firmware sleep | on | drop the bootarg |
| exynos-pmu s2idle hint | firmware treats a hinted C2 in s2idle differently | acts only in s2idle | revert; or disable `state1` at runtime |
| S2MPG12 core + RTC | IRQ storm on SPI 72; ACPM traffic at probe; RTC time written | node disabled; overlay | images without the overlay |
| `spm_lvl` 5 | slower resume (full UFS re-init) | gs201 kernel default from `a7e5e1d95` (runtime sysfs before) | `echo 3 > spm_lvl`; revert the commit |
| Wi-Fi D3 as control messages | firmware wedges after some resumes ("Failed to reserve space in commonring") | acts only in system suspend and on D2H mailbox messages | the sleep hook's unload mode (default); revert the commit |
| Sleep hook and `sleep.conf` drop-in | Wi-Fi not coming back after sleep | inert until `systemctl suspend` is used | remove the files / previous package |
| PM debug config | negligible | on in test kernels | n/a |
| Clearing `suspend-off` | users suspend a kernel that can't | only in S8, on the shipping image | set the toggle again |

## 10. Open questions

- ~~Does the default `spm_lvl` 3 survive resume?~~ No: S1 needed a host
  reset at 3, S2 was clean at 5. gs201 now defaults to 5 (`a7e5e1d95`).
- Was the S2 Volume Up wake a glitch through the analog EINT filter, or a
  touch? (3.1; S4's loop counts.)
- Do the AoC's waking services (sensor events) wake the AP in s2idle? (S6)
- Is the gauge's interrupt quiet enough to be a wake source, or should its
  wakeup be disabled from userspace?
- Does `dwc3` peripheral mode re-enumerate after resume with VBUS forced, and
  does the gadget come back without a cable replug?
- Does the RTC wake still arrive in SYS_SLEEP through the APM mailbox
  (WAKEUP2 bit 4), or through VGPIO2PMU (WAKEUP2 bit 13, not in stock's
  enable mask)? External tests saw bit 13.
- Can the S2MPG12 RTC's WTSR restart (as upstream does for S2MPG10) fix our
  hanging `reboot -f` (NOTES, "PMU and reboot")?
- With stock's own pin PDN table being sparse, which pins must we pin down
  before SYS_SLEEP (BT_REG_ON, UFS RST_N/REFCLK, Wi-Fi, haptics, touch)?

---

## Build result (2026-10-12)

- Kernel `7.3.0-rc6-omamobile-push1-00075-g025624c64c3f` (branch head
  `025624c64`) in `~/android/mainline/out-suspend`: `Image.gz`, DTBs and
  modules build (`out-suspend/build-kernel.log`, then
  `build-kernel2.log`). The second build merges
  `out-suspend/suspend-debug.config` (`PM_DEBUG`, `PM_ADVANCED_DEBUG`,
  `PM_SLEEP_DEBUG`) after `omamobile.config`.
- `sec-irq.o`, `sec-acpm.o`, `sec-common.o`, `rtc-s5m.o` and `exynos-pmu.o`
  build with `W=1` and no warnings. `CONFIG_MFD_SEC_ACPM=m`,
  `CONFIG_RTC_DRV_S5M=y`.
- `dt-doc-validate` and yamllint pass for the S2MPG12, ACPM and sysreg
  bindings. `dt-validate` of the default DTB with `suspend-pmic-test`
  applied reports only the existing `soc_id` and `/chosen/framebuffer`
  items. The default DTB has `pmic-1` disabled, the VGPIO2AP syscon and
  `mem_sleep_default=s2idle`; the overlay sets only `pmic-1` to okay.
- No boot images were made: this branch is meant to be merged into
  `omamobile-int` (it merges cleanly) and flashed from there (stage S1).

## Build result (2026-10-12, evening)

- `omamobile-suspend` head `a7e5e1d95` (UFS default) in
  `~/android/mainline/out-suspend`: kernel
  `7.3.0-rc6-omamobile-push1-00076-ga7e5e1d95e17`, Image, DTBs and modules
  build; `ufs-exynos.o` with `W=1`: no warnings; checkpatch: only the
  missing Signed-off-by and the `Co-Authored-By` trailer form.
- `omamobile-wifi-d3` head `812288601` (int `60d3ca176` + the D3 commit) in
  `~/android/mainline/out-wifi-d3`: full build passes; `pcie.o` and
  `msgbuf.o` with `W=1`: no warnings; checkpatch: the same two items plus
  three "multiple blank lines" checks, which follow brcmfmac's own style of
  two blank lines between functions.
- `git merge-tree`: int + `omamobile-suspend` + `omamobile-wifi-d3` merge
  without conflicts.
- No DT changes this round, so nothing new for `dt-validate`; the
  `suspend-pmic-test` overlay applies to the current int DTB (offline
  `fdtoverlay`, only `pmic-1` changes to okay). Note that int's
  `gs201-cheetah.dtb` is already overlaid (66946 bytes against 50720 for
  `.default`), so S3b applies the PMIC overlay on top of it and keeps
  `.pre-s3` for rollback.
- `omamobile-device-cheetah`: `test/all` passes with the new hook, link and
  drop-in (no version bump; they ship in S8).

**S3a result (2026-10-12, 7.3.0-rc6-omamobile-push1-00317-gdea700f6eec2):
passed.** With `a7e5e1d95` merged, `spm_lvl` reads 5 at boot (`rpm_lvl` 3).
Wi-Fi unloaded; one s2idle freeze slept 26 s (wall clock) and woke on Volume
Up (`pm_wakeup_irq` 106, pressed by the human); no UFS error-handler lines;
Wi-Fi came back with `systemctl start omamobile-wifi`.
