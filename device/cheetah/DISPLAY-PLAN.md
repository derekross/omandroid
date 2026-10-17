# Display driver plan: DECON0 → DSIM0 → S6E3HC4 (cheetah / gs201)

Status (2026-10-12): **our flip-only KMS driver, gs-decon, runs the display
by default** at 120 Hz (with a 60 Hz mode), with brightness over DSI and a
real screen-off. D1b, D2, D5a-c, D4a-1/2, D5d/D6 and D3b-1/D3b-2 passed on
the phone. omamobile-device-cheetah 0.1.16+ sets `gs_decon screen_off=2`
(trigger masked and the panel asleep on DPMS off; commit `1aa851d`), with
hyprland 0.56.2-5.1 (the DPMS retry) and omamobile-shell 0.1.20+ (the power
key with the panel off). Kernel: `omamobile-display` @ `43918aeb7` and
`omamobile-display2` @ `5f4808c32` (`wt-display2`, `out-display2`), both
merged into `omamobile-int`. **Next:** D3b-3 (screen-off power at each
`screen_off`), D3b-4 (soak), then D4a-3 (trigger masked between flips,
vblank from TE) and the panel's 10 Hz idle. Sections 1-5 are the original
research; the results follow in date order from "G0 status".

Earlier status (historical, superseded by the status above): D1b, D2, D5a-c and D4a-1/2 pass on the phone (brightness, flip-only
KMS under Hyprland, 120 Hz by hand). D5d/D6 (gs-decon by default, 120 Hz as a
KMS mode) is built offline in branch `omamobile-display2`
(`~/android/mainline/wt-display2`); see "D5d/D6" at the end for the design and
the staged runbook.

Goal, in order of value:

1. **Brightness** as a backlight device, so `brightnessctl`, Omarchy's
   brightness keys and a quick-settings slider work.
2. **Real screen off/on** for DPMS (lock screen, idle).
3. **KMS**: atomic commits, TE/vsync, tear-free page flips, replacing
   simpledrm; later suspend/resume and the local-HBM hook for the
   under-display fingerprint sensor.

When this plan was written, simpledrm scanned out ABL's framebuffer at `0xfac00000`, and the
initramfs writes `TRIG_CON = 0x3061` (`initramfs/init`, from `SIMPLEFB_TRIG_CON`) so
DECON0 sends a frame on every panel TE pulse (NOTES.md, "Display").

Path abbreviations:

- `K/` = `~/android/mainline/wt-display` (our kernel, branch `omamobile-display`)
- `G/` = `~/android/omamobile/out-kernel/google/gs-6.1/private/google-modules/display/samsung`
- `HC4` = `G/panel/panel-samsung-s6e3hc4.c`, `DRV` = `G/panel/panel-samsung-drv.c`
- `GS/` = `.../google-modules/soc/gs`, `CAL/` = `GS/drivers/soc/google/cal-if/gs201`
- `GDT/` = `.../gs-6.1/private/devices/google`
- `R/` = `~/android/reference/cheetah-stock-cp2a-root` (`live.dts`, `dmesg.txt`)

---

## 1. Hardware path

### 1.1 Blocks

gs201 builds `G/cal_9845/*` (DSIM, DECON core, DPP) plus `G/cal_9855/*`, and
`cal_9855/decon_reg.c:21` includes `../cal_9845/regs-decon.h`. So the 9845
register layouts apply, and gs101 shares them. The memory-map comment at
`regs-decon.h:31` (DECON_MAIN 0x1C30_0000) is wrong for gs201; the DT is right.

| Block | Base / size | IRQ (GIC SPI) | Evidence |
|---|---|---|---|
| DECON0 | main `0x1c240000`/0x6000, win `0x1c250000`, sub `0x1c260000`, wincon `0x1c270000`, dqe `0x1c2a0000` | frame_start 241, frame_done 240, extra 239 | `R/live.dts:5794-5801`; `GDT/gs201/dts/gs201-drm-dpu.dtsi:263-290` |
| DPP0..5 (RDMA) | dma `0x1c0b0000 + n·0x1000`, dpp `0x1c0d0000 + n·0x1000` | dma 262+n, dpp 271+n | `R/live.dts:5562-5647` |
| DSIM0 | link `0x1c2c0000`/0x300, dphy `0x1c2e0100`/0x700, dphy-extra `0x1c2e0000`/0x100 | 250 | `R/live.dts:5702-5708` |
| CMU_DPU / D_TZPC_DPU / SYSREG_DPU | `0x1c000000` / `0x1c010000` / `0x1c020000` | | `CAL/cmucal-sfr.c:33`; `regs-decon.h:39-42` |
| CMU_DISP / D_TZPC_DISP / SYSREG_DISP | `0x1c200000` / `0x1c210000` / `0x1c220000` | | `CAL/cmucal-sfr.c:31`; `regs-decon.h:24-27` |
| SysMMU DPU0/1/2 (`samsung,sysmmu-v8`) | `0x1c100000` / `0x1c110000` / `0x1c120000` | 280-281 / 283-284 / 286-287 | `R/live.dts:5007-5056` |
| S2MPU DPU0/1/2 (`google,s2mpu`, in pd_dpu) | `0x1c160000` / `0x1c170000` / `0x1c180000` | 282 / 285 / 288 | `R/live.dts:9514-9537` |

DPUF (front end: DMA, DPP, SysMMUs) is clocked by CMU_DPU; DPUB (DECON) by
CMU_DISP (`CAL/cmucal-qch.c:295,330-331`).

### 1.2 What ABL leaves running

| Register | Value | Meaning |
|---|---|---|
| DPP0 `RDMA_BASEADDR_Y8` (`0x1c0b0040`) | `0xfac00000` | scans a physical address, so the DPU SysMMU is most likely bypassed (D1 confirms) |
| DECON0 `GLOBAL_CON` (`0x1c240020`) | `0x133` | command mode (bit 8), RUN_STATUS (4), DECON_EN/EN_F (1:0) |
| DECON0 `TRIG_CON` (`0x1c240030`) | `0x3070` → we write `0x3061` | `0x3070` = TE edge/active bits, `HW_TRIG_MASK_DECON\|SLAVE0\|SLAVE1`, `HW_TRIG_EN = 0`. `0x3061` clears MASK_DECON and sets HW_TRIG_EN (`G/cal_9845/regs-decon.h:97-116`) |
| pd_disp `0x18062280`, pd_dpu `0x18062200` | on | stock prints "pd-disp: on", "pd-dpu: on" at probe (`R/dmesg.txt:421-422`) |

Stock does **not** keep ABL's picture: exynos-pd powers both domains off at
`sync_state` (`R/dmesg.txt:1885-1886`), and DECON0 has no `memory-region`, so
`decon_probe` starts in `DECON_STATE_INIT`, not `HANDOVER`
(`G/exynos_drm_decon.c:2729-2735`), then stops ABL's DECON and re-initialises
it on the first modeset (`:1653-1664`). DSIM probes into
`DSIM_STATE_HANDOVER` and skips `dsim_reg_init` if the PLL is already stable
(`G/exynos_drm_dsim.c:3218-3236, 325-333`). A handover path exists
(`exynos_drm_fb.c:551-574`, `decon_reg_get_win_ch`) but cheetah doesn't use it.

**Rules for anything we load:**

- Keep `pd_ignore_unused clk_ignore_unused` in the DTB bootargs
  (`K/arch/arm64/boot/dts/exynos/google/gs201-cheetah.dts:29`). There is no
  gs201 PD driver upstream (`K/drivers/pmdomain/samsung/exynos-pm-domains.c:86-89`
  knows only exynos4210/5433), so nothing turns the domains off. Do not add
  `power-domains` to display nodes until a PD driver exists **and** can restore
  the DTZPC (`need_smc` 0x1c210204 / 0x1c010204,
  `GDT/gs201/dts/gs201-pm-domains.dtsi:91-112`).
- Do not touch the DPU SysMMUs or S2MPUs. Nothing in Linux drives them today.
  If one turns out to be enabled with ABL's tables (D1 reads them), a driver
  that resets it would cut scanout.
- Never write `GLOBAL_CON` or `SRESET` while simpledrm owns the framebuffer:
  the panel keeps the last frame, but nothing restarts DECON without a
  full init.

### 1.3 Clocks

Before this work `clk-gs201.c` had only the CMU_TOP side
(`MUX/DIV/GATE_CLKCMU_DPU_NOC` and `..._DISP_NOC`, 0x1054/0x1850/0x2078 and
0x104c/0x1848/0x2070). **CMU_DPU and CMU_DISP were missing**; gs101 has
CMU_DPU upstream (`K/drivers/clk/samsung/clk-gs101.c` "CMU_DPU",
`gs101.dtsi:1893-1907`).

G0 adds both (`K/drivers/clk/samsung/clk-gs201.c`, "CMU_DISP"/"CMU_DPU"),
from `CAL/cmucal-sfr.c:415-422, 838-842, 1457-1538, 3193-3249, 4162` and the
gate parents in `CAL/cmucal-node.c:2076-2085, 2126-2157`. Differences from
gs101: BUS is called NOC, there is an extra `SLH_AXI_MI_P_DPU` gate, and most
gate offsets move by one or two words.

| Consumer | Clock |
|---|---|
| DECON ACLK | `CLK_GOUT_DISP_DPUB_ACLK_DECON` (CMU_DISP 0x2014, from `mout_disp_noc_user`) |
| DPP / DMA | `CLK_GOUT_DPU_DPUF_ACLK_DPP` 0x2018, `..._ACLK_DMA` 0x2014 |
| DPU SysMMUs | `CLK_GOUT_DPU_SYSMMU_DPUD{0,1,2}_CLK_S{1,2}` 0x206c-0x2080 |

Stock DECON/DSIM nodes have **no `clocks`** (DECON asks for optional
"aclk"/"aclk-disp" and gets NULL, `G/exynos_drm_decon.c:2634-2648`; DSIM takes
none). The CMUs run in hardware auto-gating, and the rate comes from ACPM DVFS
domain `ACPM_DVFS_DISP = 0x0B04000C` (`GDT/gs201/dts/gs201.dtsi:326`; DECON
`dfs_lv` 664/533/465/400/310/267/134 MHz, `R/live.dts:5816`). Like the GPU,
**Linux never sets DPU clock rates directly**; that needs gs201 ACPM (see
GPU-PLAN.md G4a).

Registering CMU_DPU/CMU_DISP writes `CMU_OPT_GLOBAL_EN_AUTO_GATING` to each
`CONTROLLER_OPTION` (`K/drivers/clk/samsung/clk-exynos-arm64.c:99-112`) under
a running scanout. Gate enable/disable are no-ops in auto mode
(`K/drivers/clk/samsung/clk.c:273-287`), so `clk_disable_unused` cannot gate
them. The nodes are **disabled** in G0 and get enabled in D1.

### 1.4 TE, reset, supplies

| Signal | Where | Evidence |
|---|---|---|
| TE | `gpp0-3`, function 0xF (`disp_te_pri_on`), on PERIC0 pinctrl `0x10840000` | `R/live.dts:3700-3722`, dsim `te-gpio`/`te_from = 0` `:5713-5717` |
| Reset | `gpa7-1` (FAR_ALIVE), high = running | `R/live.dts:5757-5769` (panel@3) |
| VDDI | S2MPG13 BUCKA, always-on, 1.8 V | same |
| VDDD | S2MPG12 LDO28 "L28M_DISP", 1.125 V (1.05 V in AOD) | same |
| VCI | S2MPG12 LDO27 "L27M_DISP_VCI", 3.02 V | same |

- DECON uses TE as a **hardware trigger** (`HW_TRIG_SEL_FROM_DDI0`,
  `G/cal_9845/decon_reg.c:588-610`). Google also requests the TE GPIO as a
  rising-edge IRQ, only to call `drm_crtc_handle_vblank`
  (`G/exynos_drm_decon.c:2462-2534`).
- Our DT has **no PERIC0 pin controller** yet (only PERIC1, ALIVE and
  FAR_ALIVE), so the TE GPIO isn't reachable. DECON's frame_start IRQ (SPI 241)
  is the simpler vsync source anyway.
- We have **no S2MPG12/13 regulator driver** (needs gs201 ACPM;
  NOTES.md "PMIC path"). The panel driver gets dummy regulators. That's fine
  while ABL's rails stay on, but a real panel power-off needs them.

### 1.5 DSIM command path (what brightness needs)

All in `G/cal_9845/regs-dsim.h`:

| Register | Offset | Use |
|---|---|---|
| `PKTHDR` | 0x58 | `DI[7:0] \| DATA0[15:8] \| DATA1[23:16]`, BTA bit 24 (`:233-238`) |
| `PAYLOAD` | 0x5c | 32-bit words, little-endian, written **before** the header |
| `FIFOCTRL` | 0x68 | `FULL_PH` 11, `EMPTY_PH` 10, `FULL_PL` 9, `EMPTY_PL` 8, `EMPTY_RX` 12 (`:252-259`) |
| `ESCMODE` | 0x2c | `CMD_LPDT` bit 7: commands in LP. Google clears it after init: all runtime commands go in HS (`G/cal_9845/dsim_reg.c:1568, 2434-2439`) |
| `CMD_CONFIG` | 0x80 | `PKT_GO_EN` 16 / `PKT_GO_RDY` 17: hold packets and release them in the TE window (`:302-307`) |
| `CMD_TE_CTRL0/1` | 0x84/0x88 | stable-VFP / TE-protect / TE-timeout timers that keep command packets out of frame transfers (`dsim_reg.c:1213-1231`) |
| `INTSRC` | 0x50 | `SFR_PH_FIFO_EMPTY` 28, `SFR_PL_FIFO_EMPTY` 29, `FRAME_DONE` 24 |

Google's send path (`G/exynos_drm_dsim.c:2323-2417`):

1. Write the payload words.
2. Write the header.
3. Wait for PH/PL FIFO empty (IRQ, with a FIFOCTRL fallback poll and an 80 ms
   timeout).

Batches use packet-go:

- The first queued packet sets `PKT_GO_EN`.
- The last one sets `PKT_GO_RDY`, after waiting one vblank if the frame is
  more than 95% done (`:2438-2597`).
- **Single writes are not synchronised to TE in software.** The DSIM hardware
  arbitrates them against frame transfers.

---

## 2. The panel: S6E3HC4

- **Revision:** cheetah ships `panel_rev 0x800` = PVT (`R/dmesg.txt:4701`). So
  every `<EVT1_1`/`<DVT1_1` command in HC4 is skipped, and the `≥DVT1` ones
  are used.
- **Link:** 4 lanes (`HC4:1663`), 1346 Mbps/lane (`hs-clk 0x542`,
  `pmsk 2 0xdb 2 0x1355`, esc 20 MHz, `R/live.dts:5433-5444`), non-continuous
  clock, command mode.
- **Mode:** 1440x3120 @60 (preferred) / @120, 1080x2340 @60/@120. Porches are
  h 80/24/36 and v 12/4/14 (`HC4:1341-1546`). Stock switches to 1080x2340@120
  after boot (`R/dmesg.txt:4695`). **ABL leaves WQHD @60** (our framebuffer is
  1440x3120).
- **DSC:** v1.1, 8 bpc, 8 bpp, 2 slices of 720x52 (WQHD). The PPS is a static
  128-byte table (`HC4:77-103`), sent with DSI type 0x0A after `9D 01`
  (`HC4:1057-1058`).

### 2.1 Handoff

`exynos_panel_handoff` (`DRV:806-843`) reads the reset GPIO:

- If it is high, the panel is `PANEL_STATE_HANDOFF`: the driver powers the
  regulators **without** a reset, and `is_brightness_initialized = true`.
- `s6e3hc4_enable` then skips reset, init and display-on
  (`needs_reset = !is_panel_enabled`, `HC4:1033`).
- Stock confirms it: "panel enabled at boot" (`R/dmesg.txt:2540`).

Our driver does the same.

### 2.2 Cold init (prepare)

1. VDDI, 5 ms, VDDD, VCI (`DRV:710-716`).
2. Reset H 5 ms / L 5 ms / H 10 ms (`DRV:594-637`).
3. `9D 01`, then the PPS.
4. The init table (`HC4:962-1025`). For PVT:

   ```
   11 +10ms
   F0 5A 5A
   BB 0B 04 04 41 3D 0C
   B0 00 58 94; 94 0C 60 0C 60
   E4 00 2C 2C A2 00 00
   B0 00 12 B1; B1 08
   B0 00 0E F4; F4 18 18 18 18 18
   B0 00 31 F4; F4 00 00 00 00 00
   E4 00 2C 2C 82 00 00
   F0 A5 A5 +110ms
   35
   F0 5A 5A
   B0 00 04 F6; F6 5A
   B0 02 F3 68; 68 09 09 09 0A 0A 0A
   B0 00 36 C5; C5 11 10 50 05 4E 74
   AB 83
   B0 00 3C B9; B9 19 09
   B0 00 40 B9; B9 30 03
   B0 00 05 F2; F2 C8 C0
   B0 00 10 BD; BD 00
   F7 0F
   F0 A5 A5
   2A 00 00 05 9F
   2B 00 00 0C 2F
   ```

5. `F0 5A 5A; C3 0C; F0 A5 A5` (WQHD, `HC4:1067-1073`).
6. The feature/frequency sequence (`HC4:287-497`), then `53 20`.

### 2.3 Display on/off (`HC4:780-809`)

These are Google's `PANEL_STATE_BLANK` path: the panel stays out of sleep.

```
on:  F0 5A 5A; E4 00 2C 2C A2 00 00; B0 00 4F F4; F4 70; B0 00 31 F4; F4 17 17 17 17 17;
     E4 00 2C 2C 82 00 00; F0 A5 A5; 29
off: 28; F0 5A 5A; E4 00 2C 2C A2 00 00; B0 00 31 F4; F4 00 00 00 00 00; B0 00 4F F4; F4 50;
     E4 00 2C 2C 82 00 00; F0 A5 A5
```

Sleep-in, used only on the way to power-off (`HC4:133-141`):
`10` +10 ms, `00` (NOP) +100 ms. Then reset low and regulators off
(`DRV:728-737`).

### 2.4 Brightness and HBM

- **Format:** `51 MSB LSB` (big-endian DBV, the same as mainline
  `mipi_dsi_dcs_set_display_brightness_large`; `HC4:737-778`).
- **Range:**
  - Normal: 4-2047 (2-600 nits).
  - HBM: 2900-4095 (600-1000 nits); `max_brightness 4095`, default 1023
    (`HC4:1631-1665`).
- **Brightness 0:** Google sends `22` (all pixels off) instead, and `13` before
  the next non-zero level.
- **WRCTRLD (`0x53`):**
  - BCTRL `0x20` is always set: normal = `53 20`.
  - HBM = `53 E0`.
  - LHBM = `53 30` (`HC4:717-734`).
  - No dimming bit on this panel.
- **HBM on** (`HC4:1198-1233`, IRC at `:346-363`): the IRC-off sequence
  `B0 01 9B 92; 92 01` plus `B0 02 F3 68; 68 20 1E 0C 82 82 78`, then the
  feature sequence, then `53 E0`. **Off** is the reverse.
- **Local HBM for the fingerprint sensor** (`HC4:1235-1265`): one batch
  `F0 5A 5A; B0 02 1E 92; 92 20 88 71 39 8A 01; B0 02 24 92; 92 2D 09 39;
  B0 02 27 92; 92 78; F0 A5 A5`, then `53 30`. Off is `53 20`.
  - It has a 3 s timeout (`G/panel/panel-samsung-drv.h:732`).
  - It is only allowed at peak refresh (`DRV:4870-4877`).
  - It has to land on a TE edge, so it needs packet-go or KMS.

---

## 3. Options

### (a) Keep ABL's pipeline, add a DSI command path (recommended first)

A platform driver on DSIM0 registers a `mipi_dsi_host`. It does nothing else.
`mipi_dsi_host_register()` creates the panel's `mipi_dsi_device` from the DT
child (`K/drivers/gpu/drm/drm_mipi_dsi.c:331-348`), and the panel driver
registers its backlight at probe. Brightness writes go straight to
`host->ops->transfer`; no DRM device, encoder or `drm_panel_enable` is
involved.

- **Gets:**
  - Goal 1 (brightness, 0-2047; HBM later).
  - Goal 2 in its "display off" form: `bl_power` runs the display off/on sets.
    The OLED goes dark and the panel keeps its state.
  - Scanout stays simpledrm + `TRIG_CON`.
- **Doesn't get:**
  - vsync and tear-free flips.
  - Real DECON idle.
  - Suspend.
- **Cost:** about 300 lines (done in G0). Every packet goes through two FIFO
  writes and a poll.
- **Risk:** lowest. We add writes to two FIFO registers and touch no clocks,
  no PD, no SysMMU and no DECON.
- **Why not samsung-dsim.c:** the upstream driver
  (`K/drivers/gpu/drm/bridge/samsung-dsim.c`) has no DSC (no hits), only
  stub command mode (`:1970-1974`), and refuses transfers unless the bridge
  is enabled (`:2025`). The 9845 layout also differs in every register past
  0x04. Adding a variant there is a KMS-port-sized job.
- **DPMS wiring:** Hyprland's DPMS goes to simpledrm, which can't blank.
  Omarchy's idle and lock call `hyprctl dispatch dpms off|on`. A small
  hypridle/`omarchy-hook` script also writes
  `/sys/class/backlight/*/bl_power` (4 = off, 0 = on). This is userspace, in
  the omamobile repo; brightness needs no glue (`brightnessctl` finds the
  backlight).

### (a+) Same, plus DECON trigger control

Map DECON0 main (read/modify `TRIG_CON` only).

- Mask the hardware trigger while the panel is off, so no frames are fetched
  and DPU memory traffic stops.
- Also mask it around long command batches if D2 shows glitches.
- With sleep-in (`10`), TE stops and DECON idles by itself, so this matters
  mostly for display-off without sleep.

This replaces the `devmem` line in the initramfs.

### (b) Full DECON + DPP + DSIM KMS port

A real DRM driver:

- **DECON:** CRTC, `regs-decon.h` start/stop (`decon_reg.c:1872-1944`), shadow
  update (`SHD_REG_UP_REQ` 0x50).
- **Vblank:** frame_start/frame_done IRQs (0x60/0x70).
- **DPP:** planes, RDMA at 0x00-0x54.
- **DSIM:** encoder/host with DSC slice registers (0x74-0x7c), PHY/PLL
  (`regs-dsim.h:473-489`) and packet-go.
- **Panel:** our panel driver through `drm_panel`.

Needs, beyond the driver:

1. **IOMMU or contiguous buffers.**
   - SysMMU v8 has no upstream driver (`K/drivers/iommu/exynos-iommu.c:870`
     knows v1-v7 only; Google's is `GS/drivers/iommu/samsung-iommu.c`).
   - Until then, use `drm_gem_dma` (CMA) buffers with the SysMMU in bypass
     (what ABL appears to do).
   - Allocate CMA from a `memory-region` sized for 2-3 WQHD buffers (36 MB).
2. **A PD driver** for suspend: SMC + DTZPC restore.
3. **ACPM DVFS** for DISP: ABL's fixed rate is fine at 60 Hz WQHD.
4. **S2MPU:** check it allows DPU reads of any buffer (D1).
5. **The PERIC0 pinctrl node**, if the TE GPIO is used.

Cost: weeks. Risk: medium-high. Taking DECON over without breaking the
running scanout is the hard part. The handover choice is between:

- adopting ABL's state (Google's HANDOVER path), or
- a controlled stop → reinit → start, which blanks briefly.

### (c) A stepping stone between them (D4a)

A "flip-only" KMS driver keeps ABL's DECON/DSIM/DSC configuration and only:

- programs DPP0's `RDMA_BASEADDR_Y8` (0x40) to the new buffer;
- requests a shadow update;
- signals vblank and page-flip events from DECON's frame_start/frame_done
  IRQs.

That gives tear-free atomic flips, a real vsync and zero-copy scanout (no CPU
copy into `0xfac00000`). It is a fraction of (b): one plane, one fixed mode,
the DSI host from (a) underneath. Full modesetting, 120 Hz, FHD and suspend
stay in (b).

**Recommendation:** (a) now (D1-D3), then (c) for vsync/flips (D4a). Grow (c)
into (b) as the PD, ACPM and SysMMU pieces arrive (D4b+). Each stage replaces
the previous one cleanly: the panel driver and DSIM host carry forward.

---

## 4. Bring-up gates

Rules for every gate:

- **Rollback:**
  - `panic=10` reboots on any panic.
  - Our slot B never marks itself successful, so after its retries ABL falls
    back to stock slot A (NOTES.md "Boot chain").
  - Known-good images stay in `~/android/mainline/known-good`; `bin/omamobile
    push` writes slot B from a running phone.
- **One change per boot.** Driver loads are by hand over SSH while
  `dmesg -w` runs in a second session.
- **Never on the phone from this plan without the user:** each gate below is
  a separate, user-approved step.

### Gate D0: offline (done)

- CMU_DPU/CMU_DISP clocks, bindings, the panel and DSIM-host drivers.
- DT nodes, all disabled.
- Builds cleanly, W=1 clean. `dt_binding_check` for the three schemas and
  `dt-validate` of `gs201-cheetah.dtb` report nothing new (only the
  long-standing `soc_id`).
- **Pass:** a kernel with this branch boots exactly like today. No new
  device probes; `gs-dsim-cmd` and the panel module are built but have no
  device to bind.

### Gate D1: read-only probing (low risk)

**D1a, no new kernel.** Read with `devmem`, one register per call:

| Address | Expect / learn |
|---|---|
| `0x1c2c0000` DSIM VERSION, `0x1c2c000c` LINK_STATUS1, `0x1c2c002c` ESCMODE, `0x1c2c004c` CONFIG, `0x1c2c0068` FIFOCTRL, `0x1c2c0080` CMD_CONFIG, `0x1c2c0084`/`0x88` CMD_TE_CTRL0/1 | command mode, PH/PL FIFOs empty (bits 10 and 8 set), **PKT_GO_EN clear**, CMD_LPDT clear (HS commands) |
| `0x1c100000` SysMMU DPU0 `MMU_CTRL` (+ `0x1c110000`, `0x1c120000`) | bit 0 clear = bypass (expected, since RDMA holds a physical address) |
| `0x1c160000` S2MPU DPU0 `CTRL0` (+ `0x1c170000`, `0x1c180000`) | 0 = disabled (as HSI0/HSI2 on our boot) |
| `0x1c000800`, `0x1c200800` CMU option registers | ABL's auto-gating setting, before we register the CMUs |

- **Do not read** PMU (`0x1806xxxx`) or anything outside these blocks (see
  the GPU-PLAN.md incident).
- **Risk:** an SError if a block were unclocked. Unlikely, since all of them
  are in the scanning-out pipeline. Recovery is reboot by `panic=10`.

**D1b, new DTB.**

1. In the cheetah DTS, set `&dsi0 { status = "okay"; }` and `&cmu_dpu`/
   `&cmu_disp` okay. Blacklist `gs_dsim_cmd` and `panel_samsung_s6e3hc4` first.
2. Boot.
3. `modprobe gs-dsim-cmd`. It logs the DSIM registers (reads only).
4. `modprobe panel-samsung-s6e3hc4`. It must log "taken over from the
   bootloader" and create `/sys/class/backlight/<dsi>`.
5. `cat /sys/kernel/debug/clk/clk_summary | grep -E 'dpu|disp'` shows the new
   clocks.

- **Pass:** the screen is unchanged throughout.
- **Risks:**
  - CMU registration flips the CMUs to auto gating (medium-low; if the
    screen freezes, drop the CMU nodes again — nothing needs them in D2/D3).
  - The reset GPIO request on gpa7-1. The driver keeps it as-is and only
    re-drives the level it read.

### Gate D2: DCS brightness (low-medium risk)

1. `devmem 0x1c240030 0x3070` to mask the trigger (freezes the
   current frame, as before our init), so no frame transfer competes.
2. `echo 1 > /sys/module/gs_dsim_cmd/parameters/allow_tx`.
3. `echo 300 > /sys/class/backlight/*/brightness`. The panel must dim, then
   go back to 1023.
4. Unmask the trigger (`0x3061`).
5. Repeat with the trigger running.

- **Watch for:**
  - a FIFO timeout (logged with a register dump: no harm);
  - a single-frame glitch;
  - a black panel (it lost state; reboot, and ABL re-inits it).
- **Expected failure modes:** `PKT_GO_EN` left set (driver returns -EBUSY), or
  LP/HS mismatch (no effect on screen; then set `ESCMODE.CMD_LPDT` for
  commands).
- **Later in D2:** HBM above 2047 (the IRC + `53 E0` sequence, needs the
  feature sequence of `HC4:287-497`), `systemd-backlight` restore, and the
  quick-settings slider.

### Gate D3: screen off/on (medium risk)

- **D3a:** `echo 4 > bl_power` runs the display-off set; `echo 0` runs the
  display-on set. Then wire it into Omarchy's DPMS hook. With the trigger
  still unmasked, DECON keeps sending frames to a dark panel; that's
  harmless.
- **D3b:** add sleep-in (`10`) and mask/unmask the DECON trigger (option
  a+). Measure the battery current drop with the MAX77759 fuel gauge
  (`current_now`). **Sleep-in loses the panel's registers** (Google's
  `s6e3hc4_disable` clears its cached `hw_feat` and resets `hw_vrefresh` to
  60 "after disabling hardware", `HC4:1123-1126`, and its enable re-sends the
  whole init and feature set after any reset). So wake is always reset +
  `prepare` (full init, now including the feature/rate set, D5) + `enable`,
  never a bare sleep-out (`11`). Until KMS (D4) owns the panel, keep D3a.
- **Risk:** the panel doesn't come back. Recovery is reboot; for the user,
  that costs the session. Test only from SSH first, never from the lock
  screen.

### Gate D4: KMS (medium-high risk, staged)

- **D4a, flip-only KMS (option c):**
  - One CRTC, one primary plane on DPP0, CMA buffers.
  - vblank from DECON frame_start (SPI 241).
  - simpledrm is dropped once this binds (`drm_aperture` handover of
    `0xfac00000`).
  - Pass: `kmscube`, then Hyprland with no tearing and with vsync.
- **D4b, full DECON/DSIM init:**
  - stop → reinit → start under KMS, so a cold panel prepare works (the
    panel driver already has `prepare`/`unprepare`);
  - FHD and 120 Hz modes;
  - packet-go batches on TE.
- **D4c, suspend/resume:**
  - needs the gs201 PD driver with SMC/DTZPC restore, S2MPG12/13 regulators
    (ACPM), and SysMMU/S2MPU state restore;
  - then the power button maps to screen off + suspend (NOTES.md TODO).
- **D4d, LHBM** for the fingerprint sensor: packet-go batch on a TE edge.

---

## 5. Risks

1. **Command/frame contention in D2.** Single writes rely on the DSIM's
   hardware arbitration (`CMD_TE_CTRL` timers set by ABL). Mitigation: test
   with the trigger masked first. If writes glitch, add packet-go (`PKT_GO_EN`
   + `PKT_GO_RDY`) in the host.
2. **No regulators.** The panel driver can't really cut VCI/VDDD. Display-off
   is the deepest state until ACPM + S2MPG12 regulators exist. Don't call
   `unprepare` (reset low) without them: the panel would then need a cold
   init, which only D4b provides.
3. **SysMMU/S2MPU state is unverified.** D1a reads it before anything depends
   on it.
4. **Two drivers for one compatible later.** `gs-dsim-cmd` binds
   `google,gs201-dsim`; the KMS driver will replace it, not coexist with it.
5. **simpledrm's CPU copy remains until D4a.** GPU rendering already works
   (GPU-PLAN.md G2); scanout is the bottleneck.

---

## G0 status (2026-10-11, offline; nothing was run on the phone)

> **Historical.** Superseded by the D1a, D1b/D2 and D6 results below and the
> status at the top.

### Kernel: `omamobile-display` in `wt-display`, on `omamobile` @ `a3061be71`

| Commit | What |
|---|---|
| `9f05eb2ab` | dt-bindings: clock: google,gs201: add CMU_DISP and CMU_DPU |
| `87d9f6f8d` | clk: samsung: gs201: add CMU_DISP and CMU_DPU |
| `75e45955f` | dt-bindings: display: panel: add Samsung S6E3HC4 |
| `8e5aab2a4` | drm/panel: add Samsung S6E3HC4 command-mode panel |
| `ac74b32ef` | dt-bindings: display: samsung: add Google gs201 DSIM |
| `88d66e20f` | drm: google: add a gs201 DSIM command path for the bootloader display |
| `d4c18e5a9` | arm64: dts: gs201: add the display CMUs and DSIM0 (all disabled) |
| `b32b68663` | arm64: dts: gs201-cheetah: put the panel on DSIM0 |

**`gs-dsim-cmd`** (`K/drivers/gpu/drm/google/gs-dsim-cmd.c`):

- Maps DSIM0 and logs 13 registers at probe.
- Registers a `mipi_dsi_host`.
- Writes go through the payload and header FIFOs, polled up to 80 ms. Reads
  return -EOPNOTSUPP.
- Refuses to send if `PKT_GO_EN` is set.
- Sends nothing unless `allow_tx=1`.

**`panel-samsung-s6e3hc4`** (`K/drivers/gpu/drm/panel/panel-samsung-s6e3hc4.c`):

- At probe it reads the reset line as-is. If the panel is released, it takes
  the panel over silently.
- Registers backlight 0-2047 (non-linear, default 1023): 0 → `22`, then `13`;
  `bl_power` → the display off/on sets.
- `drm_panel` prepare/enable/disable/unprepare carry the full PVT cold init
  for D4, plus DSC config for a future host.

**Builds:**

- `bin/omamobile kernel/modules/initramfs/images` with
  `KERNEL_OUT=~/android/mainline/out-display` all pass; logs are
  `logs/*-display.log`.
- W=1 is clean for the three changed objects.
- Release `7.3.0-rc6-omamobile-push1-00053-gb32b686639d8`; images in
  `out-display/images`.
- DT in this state: `dsi0`, `cmu_dpu` and `cmu_disp` are disabled, so nothing
  new probes.

**omamobile repo:** `kernel/configs/omamobile.config` gains
`CONFIG_DRM_GS_DSIM_CMD=m` and `CONFIG_DRM_PANEL_SAMSUNG_S6E3HC4=m`. That file
also holds someone else's uncommitted Wi-Fi lines, so it's left for them to
commit together.

### D1 runbook (needs the user; phone in Arch over USB)

```sh
# D1a: read-only, current kernel (one register per call)
for a in 0x1c2c0000 0x1c2c000c 0x1c2c002c 0x1c2c004c 0x1c2c0068 0x1c2c0080 \
         0x1c2c0084 0x1c2c0088 0x1c100000 0x1c110000 0x1c120000 \
         0x1c160000 0x1c170000 0x1c180000 0x1c000800 0x1c200800; do
  tools/phone-ssh "devmem $a"; done
# (devmem = tools/devmem.c, the one the initramfs uses: devmem <addr> [value])
# D1b: blacklist first, then a DTB with dsi0 (and the CMUs) okay. Not built
# yet: add to gs201-cheetah.dts
#   &dsi0 { status = "okay"; };  &cmu_dpu { status = "okay"; };  &cmu_disp { status = "okay"; };
# and rebuild kernel + modules + images with KERNEL_OUT=out-display
tools/phone-ssh 'printf "blacklist gs_dsim_cmd\nblacklist panel_samsung_s6e3hc4\n" \
  > /etc/modprobe.d/omamobile-display.conf'
export KERNEL_OUT=$HOME/android/mainline/out-display
bin/omamobile modules-push && bin/omamobile push
tools/phone-ssh dmesg -w            # second terminal
tools/phone-ssh 'modprobe gs-dsim-cmd; modprobe panel-samsung-s6e3hc4; ls /sys/class/backlight'
```

### Open questions

- Whether ABL leaves `PKT_GO_EN` and `CMD_LPDT` clear (D1a).
- Whether the DPU SysMMU is bypassed and the S2MPU off (D1a).
- ~~Whether sleep-out alone restores the picture (D3b).~~ It doesn't: sleep-in
  resets the panel's registers; wake re-sends the full init (D3b, D5).
- The exact DBV ABL sets at boot. The driver assumes 1023 and doesn't read it
  back (DCS reads need BTA/RX support in the host).

## D1a result (2026-10-11, on the phone, read-only)

All 16 reads returned cleanly (no SError):

| Register | Value | Reading |
|---|---|---|
| DSIM VERSION `0x1c2c0000` | 0x02060000 | |
| LINK_STATUS1 `0x1c2c000c` | 0x04000000 | |
| ESCMODE `0x1c2c002c` | 0x01400000 | CMD_LPDT (bit 7) clear: HS commands; no ULPS, no forced stop |
| CONFIG `0x1c2c004c` | 0x04887cff | |
| FIFOCTRL `0x1c2c0068` | 0x00005507 | RX, PH and PL FIFOs empty (bits 12, 10, 8); none full |
| CMD_CONFIG `0x1c2c0080` | 0x00000000 | **PKT_GO_EN clear**: the command path is free |
| CMD_TE_CTRL0/1 `0x1c2c0084/88` | 0x00000056 / 0x33eb3b26 | |
| SysMMU DPU0-2 `MMU_CTRL` | 0 | bypass, as expected |
| S2MPU DPU0-2 `CTRL0` | 0 | **disabled**: the DPU's DMA is unconfined, like HSI0 and HSI2 (the S2MPU item on the pre-ship security list) |
| CMU_DPU / CMU_DISP option `+0x800` | 0xf1000000 | ABL's setting, to compare after D1b registers the CMUs |

Both open questions about ABL's DSIM state are answered in the driver's favour: D1b can proceed.

## D1b and D2 results (2026-10-11, on the phone)

- **D1b passes.** With `dsi0`, `cmu_dpu` and `cmu_disp` okay: both CMUs register (52 dpu/disp clocks in `clk_summary`) and their option registers still read 0xf1000000 (no auto-gating change, picture unaffected). `gs-dsim-cmd` attaches and dumps an idle DSIM (FIFOs empty, PKT_GO_EN clear); `panel-samsung-s6e3hc4` logs "taken over from the bootloader" and creates `/sys/class/backlight/1c2c0000.dsi.0` (1023 of 2047).
- **Found in D1b:** with `dsi0` enabled, fw_devlink made simplefb wait for the panel device (the `panel` phandle that gives simplefb the panel size), which only appears with the DSI host driver. simpledrm probed late, so the boot splash and Plymouth had no framebuffer and Hyprland started without a display. Fixed with `post-init-providers = <&panel>` on the framebuffer node.
- **D2 passes.** With `allow_tx`: brightness 300 then 1023, first with the DECON trigger masked, then with it running. Every write returned 0, no FIFO timeout or -EBUSY, FIFOs empty and PKT_GO_EN clear afterwards, and the user saw the panel dim. HS commands work as ABL left them (no CMD_LPDT needed).
- Next: make `allow_tx` the default once D3 passes, a quick-settings brightness slider on `/sys/class/backlight`, `systemd-backlight` restore, then D3 (display off/on for DPMS).

## D5: smooth display (2026-10-12, offline; nothing was run on the phone)

Goal: Hyprland with real vsync, tear-free zero-copy flips, then 120 Hz.
Kernel branch `omamobile-display2` (`~/android/mainline/wt-display2`, out dir
`out-display2`), on top of the G0/D1b commits. Abbreviations as in the top
of this file, plus `DSIM` = `G/exynos_drm_dsim.c`, `DEC` =
`G/exynos_drm_decon.c`, `BTS` = `G/exynos_drm_bts.c`.

### D5.1 Which rate ABL configured, and how to measure the real one

**ABL runs the panel at 60 Hz** (to be confirmed by D5a):

- ABL's DSIM TE timers are the 60 Hz ones. Google programs
  `CMD_TE_CTRL1 = TE_PROTECT_ON << 16 | TE_TOUT` from
  `hs_clk * (100 - 5) * 100 / fps / 16` and `hs_clk * (100 + 10) * 100 / fps / 16`
  (`G/cal_9845/dsim_reg.c:1206-1231`, `TE_MARGIN` 5). With our 1346 Mbps lanes
  that is 13319/15422 at 60 Hz and 6659/7711 at 120 Hz. ABL left
  `0x33eb3b26` = 13291/15142 (D1a): the 60 Hz pair, within 2%.
- Google cold-starts the panel at 60 Hz (`vrefresh = needs_reset ? 60`,
  `HC4:1043`) and ABL leaves WQHD.
- The panel itself can run its TE at 120 Hz while refreshing at 60 ("fixed
  TE" with early exit, `HC4:332-338`), and DECON sends one frame per TE
  while its trigger is unmasked. So the number that matters is what
  DECON actually sends; measure it.

**Method 1, read-only, no new kernel: `tools/display-hz`.** DECON has a
free-running `FRAME_COUNT` register (`0x1c240004`, `G/cal_9845/regs-decon.h:80`).
The tool reads it (and `HW_TE_CNT` 0x38) twice on the phone, N seconds apart,
and prints frames per second, together with the DSIM registers that depend
on the rate: `UNDERRUN_CTRL` (0x34), `CMD_TE_CTRL0/1` (0x84/0x88) and the
`INTSRC` underrun bit 19 (`G/cal_9845/regs-dsim.h:129,202,310-319`). Nothing
is written unless `--clear-underrun` is given (one write-one-to-clear of
`INTSRC` bit 19). DECON0 and DSIM0 are on and clocked whenever the screen
shows anything, and D1a already read these blocks safely.

**Method 2, interrupt counter: `gs-decon stage=1`.** If `FRAME_COUNT` does
not move in command mode (Google never reads it), the new driver counts the
DECON frame start (SPI 241) and frame done (SPI 240) interrupts and reports
`/sys/bus/platform/devices/1c240000.display-controller/stats`
(`frame_start_mhz` = rate in mHz). It enables only those two interrupt bits
(`DECON_INT_EN` 0x60 bits 0, 12, 13; restored on unbind), clears only its own
pending bits, and disables its interrupts if more than 2000 arrive in a
second.

Expected readings:

| Reading | Means |
|---|---|
| ≈ 60.0 frames/s, `UNDERRUN_CTRL` ≈ 0x1f09 | ABL at 60 Hz, changeable TE (the expected case) |
| ≈ 120 frames/s at 60 Hz timers | the panel runs a fixed 120 Hz TE: every frame is sent twice; worth knowing for power |
| 0 frames/s | the trigger is masked (`TRIG_CON` 0x3070): our init didn't run |

`UNDERRUN_CTRL` (the command-mode underrun LP reference) comes from
`dsim_calc_underrun` (`DSIM:2879-2922`): with 4 lanes at 1346 Mbps, DSC
(1/3 width), 3120 lines, `te_idle_us` 350 and `te_var` 1 (`HC4:1324-1327`)
one frame needs 6.71 ms on the link; the count is 7945 (0x1f09) at 60 Hz and
1004 (0x3ec) at 120 Hz.

### D5.2 What 120 Hz needs

| Part | 60 Hz (today) | 120 Hz | Source |
|---|---|---|---|
| Panel | changeable TE, no early exit, `60 01` | fixed TE (`B9 51`), early exit (`BD 21 01 83 03 03`), `60 00` | `HC4:287-497, 523-575` |
| DSIM `UNDERRUN_CTRL` 0x34 | 0x1f09 | **0x3ec** | `DSIM:2879-2922`, `dsim_reg_set_rr_config` `dsim_reg.c:2177` |
| DSIM `CMD_TE_CTRL1` 0x88 | 0x33eb3b26 (ABL) | **0x19f51d93** (ABL's values halved) | `dsim_reg.c:1206-1231` |
| DSIM PLL, PHY, DSC, `CMD_TE_CTRL0` | unchanged | unchanged | Google's seamless check refuses only clock, op-mode and DSC changes (`DSIM:1322-1350`) |
| DECON | nothing: it sends one frame per TE | nothing | `DEC:1092-1213` |
| DISP clock (DECON ACLK) | ≥ 148 MHz | **≥ 297 MHz → the 310 MHz DVFS level** | `BTS:59`: `w * h * fps * 1.1 / ppc`, ppc 2; levels 664/533/465/400/310/267/134 MHz (`R/live.dts:5816`) |
| Memory bus (MIF) | DPP0 reads 1.08 GB/s | **2.16 GB/s; reportedly underruns below MIF 1352 MHz** | `BTS:76-79`; stock MIF levels 421 … 3172 MHz incl. 1352 (`R/dmesg.txt:1175-1190`) |

**Panel sequence.** Google sends the whole feature set on every enable and
the changed parts on a rate change, ending with `F7 0F` (frequency update,
taken at the next frame). The panel driver now carries it for PVT panels,
HS operation, HBM off, IRC on and manual rate (`s6e3hc4_write_rate`):

```
F0 5A 5A
B0 00 04 B9; B9 0C 42 00 1B 0C 42 00 1B 0C 42 00 1B      TE width (HS)
B9 51 (120) | B9 04 (60)                                  fixed / changeable TE
B0 00 42 F2; F2 0D; B0 00 01 B9; B9 51|04;                TE2, same option,
B0 00 22 B9; B9 00 10 00 30 00 10 00 30 (120)              rising 0x10, falling 0x30
  | B0 00 1E B9; B9 00 10 00 30 (60)
B0 01 9B 92; 92 21; B0 02 F3 68; 68 09 09 09 0A 0A 0A      IRC on
F2 01; 60 00                                              HS operation
BD 21 01 83 03 03 (120) | BD 21 81 83 03 03 (60)          early exit on/off
B0 00 1E BD; BD 00 00 00 02 00 06 00 16
BD 21; 60 00 (120) | 60 01 (60)                           manual rate
F7 0F; F0 A5 A5
```

`prepare` now sends it (at 60 Hz) after the cold init, as Google's enable
does; before, a cold init left the TE and rate registers at the panel's
reset values. A bring-up knob, `refresh_rate` in the panel's sysfs directory,
sends it at run time, only with `panel_samsung_s6e3hc4.allow_rate_switch=1`.

**Order** (Google does both halves inside one atomic commit):

- Up: raise MIF and DISP floors → DSIM `UNDERRUN_CTRL`, `CMD_TE_CTRL1` →
  panel sequence. The panel takes the rate at its next frame.
- Down: panel sequence → DSIM timers → drop the floors.
- The DSIM timers gate command packets around TE, so with 60 Hz timers at a
  120 Hz TE every other TE may be ignored (we'd measure 60). Write them back
  to back with the panel switch.

**Memory bandwidth and ACPM.** At WQHD 120 Hz DPP0 alone reads 2.16 GB/s,
real-time, and an underrun shows as flashing horizontal lines and DSIM
`INTSRC` bit 19. Google never votes a fixed MIF: the DPU votes bandwidth
through `bts.ko` (`BTS:896-1000`) and the MIF devfreq turns that into a
frequency. We have neither, so whatever MIF ABL left is what we get; it is
reportedly 1352 MHz or more after a normal boot, but lower after other boot
paths, so it can't be relied on.

**Clean dependency on ACPM (built in `wt-acpm` by another worker).**
Upstream exposes the ACPM DVFS domains as clocks (`drivers/clk/samsung/clk-acpm.c`,
gs101: `mif` = 0, `disp` = 12, `dt-bindings/clock/google,gs101-acpm.h`).
The DECON binding takes them now as optional `clocks = <mif>, <disp>`
(`clock-names = "mif", "disp"`); `gs-decon` gets them with
`devm_clk_get_optional` and logs their rates. When gs201 ACPM lands:

1. add `clocks = <&acpm_ipc GS201_CLK_ACPM_DVFS_MIF>, <&acpm_ipc GS201_CLK_ACPM_DVFS_DISP>`
   to `&decon0` (the IDs are the ACPM worker's; on gs201 DISP is domain
   `0x0B04000C`, `GDT/gs201/dts/gs201.dtsi:326`);
2. `gs-decon` holds `clk_set_min_rate(mif, 1352 MHz)` and
   `clk_set_min_rate(disp, 310 MHz)` while the 120 Hz mode is active, and
   0 at 60 Hz and at DPMS off. A floor (not `clk_set_rate`) leaves room for
   the GPU and others to vote higher;
3. without the clocks, the driver never offers 120 Hz.

The 1352 MHz floor is a starting point: gate D5c sweeps the MIF levels and
keeps the lowest one with no underrun over a minute of full-screen motion,
plus one level of margin.

**Where the 120 Hz switch will live.** In `gs-decon` stage 2 the mode switch
is a KMS mode set (1440x3120@120 in the connector's list). It needs the
panel sequence and the DSIM timers in one commit, so `gs-dsim-cmd` and the
panel are then driven from the KMS driver (DSIM as an encoder/bridge, panel
through `drm_panel` with a rate hook); that is the first piece of option
(b). Until then the switch is by hand (D5c).

### D5.3 Flip-only KMS design (`gs-decon`, option c)

`drivers/gpu/drm/google/gs-decon.c`, binding `google,gs201-decon.yaml`, node
`decon0` in `gs201.dtsi` (disabled) and `&decon0` in the cheetah DTS
(disabled), with a 92 MiB reusable CMA pool below 4 GiB (`display_pool`,
disabled; five 1440x3120 buffers).

What it touches:

| Register | Access | When |
|---|---|---|
| DECON main (`0x1c240000`) | read | probe (dump, checks) |
| DPP0 RDMA (`0x1c0b0000`), incl. shadow copy at +0x400 | read | probe |
| `DECON_INT_EN` 0x60, `DECON_INT_PEND` 0x70 | RMW bits 0/12/13; W1C bits 12/13 | stage ≥ 1 |
| `RDMA_BASEADDR_Y8` 0x40, `SHD_REG_UP_REQ` 0x50 | write | stage 2, each flip |
| `RDMA_IN_CTRL_0` format field | write once | stage 2 with `force_xrgb=1` only |

It never writes `GLOBAL_CON`, `TRIG_CON`, clocks, power domains, the DSIM
or the panel, so DECON is never stopped and the DPU never powered down
under it. **Rule for every later stage:** before anything may power the DPU
or DISP domain off, disable the windows and stop DECON (mask the trigger,
clear `DECON_EN_F` with a global shadow update, wait for `RUN_STATUS` to
clear: `decon_reg_stop_perframe_dsi`, `G/cal_9845/decon_reg.c:1468-1489`).
The APM firmware's DPU power-off handshake otherwise times out and its
watchdog resets the SoC.

**Flip** (Google's command-mode commit, `DEC:1092-1213`,
`decon_reg_start` `decon_reg.c:1872`):

1. Plane check: the framebuffer must have the boot layout (1440x3120,
   pitch 5760, XRGB8888, no offset), a CMA address below 4 GiB (DPP0
   takes 32 bits; the DPU SysMMU is bypassed, D1a).
2. Plane update: write `RDMA_BASEADDR_Y8`, then set `SHD_REG_UP_REQ` =
   all windows | global | compressor (0x8010003f). DECON latches the new
   registers at the next frame start and clears the request bits
   (`decon_reg_wait_update_done_timeout`, `decon_reg.c:2063`).
3. CRTC flush: the page-flip event is armed, with a vblank reference.
4. Frame start interrupt: `drm_crtc_handle_vblank`; if the request bits
   are clear, the new buffer is being scanned, so the event is sent and the
   old buffer may be reused (Google sends the event at frame start too,
   `DEC:2103-2125`).
5. If no frame start takes the update within 100 ms (TE stopped, trigger
   masked), the event is sent anyway and a warning logged, so a compositor
   never stalls (Google forces the event on a frame start timeout,
   `DEC:1810-1880`).

**Vblank** is DECON's frame start. With the trigger permanently unmasked
(`TRIG_CON` 0x3061, as today) DECON sends a frame on every TE, so frame start
is a steady panel-rate vblank, and the timestamp is taken in the interrupt.
Google instead masks the trigger after each flip and uses the TE GPIO for
vblank (`DEC:1867-1871, 2462-2490`): that saves the DPU memory traffic of
re-sending unchanged frames. That is a later power step (D4a-3); it needs
either the TE GPIO (no PERIC0 pin controller yet) or DECON's own TE edge
interrupts (`DECON_INT_EN_EXTRA` bits 16/20, on the "extra" SPI 239, already
in the binding).

**Pixel format.** Only XRGB8888. Google maps `DRM_FORMAT_XRGB8888` to DPP
DMA format 7 (`G/exynos_drm_format.c:76-79`), where the alpha byte is
ignored. ABL's format is logged at probe (`RDMA_IN_CTRL_0` bits 13:8). If it
isn't 7, stage 2 refuses to start unless `force_xrgb=1`, which writes 7 with
the first flip; otherwise a client's undefined X byte could act as alpha.

**Buffers.** `drm_gem_dma` from `display_pool` (`memory-region-names =
"pool"`), write-combined, so CPU and GPU writes need no cache maintenance
before scanout. Mesa's kmsro allocates Panthor's scanout buffers as dumb
buffers on this device, so they land in the pool too.

**Parking.** Whenever no DRM buffer may be scanned (plane or CRTC disable,
unbind, shutdown), DPP0 is pointed back at ABL's framebuffer (`0xfac00000`,
reserved `no-map`, `memory-region-names = "boot-framebuffer"`), cleared to
black, and the driver waits for the shadow update before DRM frees
anything. DPMS off in this stage is therefore "black frame, DECON still
running"; the panel goes dark through the D3 `bl_power` hook as today.

**Coexisting with simpledrm, the boot splash and Plymouth.**

- simpledrm stays built in and owns the screen from early boot (fbcon,
  Plymouth), unchanged.
- `gs-decon` checks the hardware first (DECON on in command mode, DPP0
  scanning the reserved boot framebuffer at full size), and only then calls
  `aperture_remove_conflicting_devices` for `0xfac00000`. That unplugs
  simpledrm the way any native driver replaces a firmware framebuffer. It
  then registers its own DRM device and fbdev emulation, so fbcon moves
  over. Until the first commit DPP0 keeps scanning ABL's buffer, so the
  picture doesn't change at probe.
- If any check fails, simpledrm is left alone and the system boots as
  today.
- Order for daily use, once D4a passes: load `gs-decon stage=2` from the
  initramfs, before Plymouth starts, so Plymouth and Hyprland only ever see
  one card. Loading it later works too, but a running Plymouth loses its
  device.

**Panel on/off around KMS.** The panel stays with `gs-dsim-cmd` and the
panel driver (D2/D3). After a sleep-in the panel must get the full init
(reset, `prepare`, `enable`), never a bare sleep-out (D3b). That needs
KMS to drive the panel, i.e. D4b.

### D5.4 Staged runbook (each step needs the user; one change per boot)

Before anything: `tools/phone-ssh dmesg -w` in a second terminal. Recovery
as in section 4 (panic=10, slot A fallback). These reads and writes touch
only DECON0, DPP0 and DSIM0, which are on and clocked while the screen is
lit.

**D5a: measure, current kernel, read-only.**

```sh
tools/display-hz --secs 10        # three runs: idle, while scrolling, while a video plays
```

Pass: a stable rate (expect 60.0), `UNDERRUN_CTRL` ≈ 0x1f09, underrun bit 0.
If `FRAME_COUNT` doesn't move, go to D5b for the interrupt counter.
Also `tools/phone-ssh 'grep -E "disp|dpu" /sys/kernel/debug/clk/clk_summary'`
for the DISP/DPU clock rates ABL left (D5.2 needs ≥ 297 MHz for 120 Hz).

**D5b: new kernel, `gs-decon` stage 0 then 1.**

1. Build `out-display2` (kernel, modules, images) and install them. With
   the shipped DTB nothing new probes (decon0 disabled): boots exactly as
   today.
2. Test DTB: in `gs201-cheetah.dts` set `&decon0 { status = "okay"; };`
   and `&display_pool { status = "okay"; };`, rebuild `images`, and
   blacklist first:
   `tools/phone-ssh 'echo "blacklist gs_decon" > /etc/modprobe.d/omamobile-decon.conf'`.
3. Boot, then `tools/phone-ssh modprobe gs-decon` (stage 0): it logs 19
   registers. Expect `GLOBAL_CON` 0x133, `TRIG_CON` 0x3061,
   `RDMA_BASEADDR` and `RDMA_BASEADDR*` 0xfac00000, `RDMA_SRC_SIZE`
   0x0c3005a0, and note `RDMA_IN_CTRL_0` (format).
4. `rmmod gs-decon; modprobe gs-decon stage=1`, wait 10 s, then
   `cat /sys/bus/platform/devices/1c240000.display-controller/stats` and
   `grep gs-decon /proc/interrupts`. Pass: `frame_start_mhz` ≈ 60000 (or
   what D5a measured), `frame_done` ≈ `frame_start`, picture unchanged.
   `rmmod` restores `INT_EN`.

- Risk: an interrupt that can't be cleared. The driver turns its
  interrupts off after 2000 in a second, and rmmod restores `INT_EN`.

**D4a-1: flip-only KMS on the console (stage 2).** Hyprland stopped
(`systemctl stop` the session / log out to the console).

1. `modprobe gs-decon stage=2` (add `force_xrgb=1` if stage 0 showed a
   format other than 7). Expect "flip-only KMS on DECON0", simpledrm gone
   from `/sys/class/drm`, fbcon redrawn on the new card.
2. `modetest -M gs-decon -s <connector>@<crtc>:1440x3120 -v` (page-flip
   test, prints the flip rate; expect ≈ 60) and `kmscube -D /dev/dri/cardN`.
   Watch `stats`: `flips` grows, `flip_timeouts` stays 0.
3. `rmmod gs-decon`: the screen parks on black; reboot to get simpledrm
   back.

- Pass: no tearing in kmscube, a steady 60 fps, no timeouts.
- Risks: wrong pixel order (colours swapped: then the DPP format needs the
  BGRA/XBGR variant, logged at probe); a stale shadow request (each flip
  then times out after 100 ms: visible in `flip_timeouts`).

**D4a-2: Hyprland on gs-decon.** Options in
`/etc/modprobe.d/omamobile-decon.conf` (`options gs_decon stage=2`), loaded
from the initramfs before Plymouth. Pass: Hyprland's `debug:overlay` shows
60 fps with vsync, no tearing when scrolling, and `hyprctl monitors` shows
the real 60 Hz mode with vblank. Measure the drop in CPU use against
simpledrm's copy.

**D5c: 120 Hz by hand (after D4a-1, and only with the MIF floor
available, or with D5a showing a high MIF).**

```sh
P=/sys/bus/mipi-dsi/devices/1c2c0000.dsi.0
tools/phone-ssh "echo 1 > /sys/module/panel_samsung_s6e3hc4/parameters/allow_rate_switch"
# up: DSIM timers and the panel back to back (MIF/DISP floors first, once ACPM exists)
tools/phone-ssh "devmem 0x1c2c0034 0x3ec; devmem 0x1c2c0088 0x19f51d93; echo 120 > $P/refresh_rate"
tools/display-hz --clear-underrun --secs 10     # expect ~120, underrun bit 0
# down
tools/phone-ssh "echo 60 > $P/refresh_rate; devmem 0x1c2c0088 0x33eb3b26; devmem 0x1c2c0034 0x1f09"
```

- Pass: 120 frames/s, no underrun bit after a minute of full-screen motion,
  no flashing lines.
- With ACPM: repeat at each MIF level from 845 MHz up and keep the lowest
  clean one plus one level.
- Risks: underrun at low MIF (flashing lines, recoverable by going back
  down); a TE/timer mismatch (stays at 60, or the DSIM TE timeout fires;
  restore the 60 Hz values). Brightness writes keep working, but the LP
  window between frames shrinks to about 1.2 ms.

**D5d: 120 Hz under KMS.** Fold D5c into a `gs-decon` mode set (D5.2, "where
the 120 Hz switch will live"), with the MIF/DISP floors, and add 10/30 Hz
idle (the panel's auto mode, `HC4:435-481`) later.

### D5.5 Built (2026-10-12)

Kernel branch `omamobile-display2`, on `omamobile` @ `e7cb10fe5`:

| Commit | What |
|---|---|
| `1ca875861` | drm/panel: samsung-s6e3hc4: set the operating features and refresh rate (feature/rate set in `prepare`; `refresh_rate` knob behind `allow_rate_switch`) |
| `1d567d206` | dt-bindings: display: samsung: add Google gs201 DECON |
| `96f810ca9` | drm: google: add a flip-only KMS driver for the gs201 DECON (`gs-decon`, `stage` 0/1/2, `force_xrgb`) |
| `00e49c41b` | arm64: dts: exynos: google: gs201: add DECON0 (disabled) |
| `52ec674f1` | arm64: dts: exynos: google: gs201-cheetah: add the DECON0 scanout pool (disabled) |

- `kernel`, `modules`, `initramfs` and `images` build into `out-display2`
  (logs `logs/*-display2.log`), release
  `7.3.0-rc6-omamobile-push1-00060-g52ec674f1ef2`; W=1 is clean for `gs-decon.o`,
  `gs-dsim-cmd.o` and `panel-samsung-s6e3hc4.o`.
- `dt-doc-validate` passes for the new binding; `dt-validate` of the
  shipped `gs201-cheetah.dtb`, and of a D4 test DTB with `decon0` and
  `display_pool` okay, reports only the long-standing `soc_id` and the
  framebuffer's `post-init-providers`.
- With the shipped DTB nothing new probes. The one behaviour change on
  today's path: a panel cold init (`prepare`, only reachable from D4b) now
  ends with the feature/rate set.
- `kernel/configs/omamobile.config` gains `CONFIG_DRM_GS_DECON=m`.
  `tools/display-hz` is the D5a tool.

**First step:** D5a, `tools/display-hz --secs 10` on the running phone
(read-only, no new kernel).

## D5d/D6: gs-decon by default, 120 Hz as a KMS mode (2026-10-12, offline; nothing was run on the phone)

Starting point (on the phone, kernel `omamobile-int` `…-00175-g7bee66237c8d`
with `overlays/display-decon-test.dtso`): D5b and D4a-1/2 pass (60.1 Hz flips,
no flip timeouts, Hyprland on `card2` with correct colours after `a2a2ee90d`
and `fa6d7aec8`), and D5c ran 120.02 Hz under Hyprland with the DSIM timers
written by hand. This step makes that the default.

### D6.1 What changed

Branch `omamobile-display2`, on top of `fa6d7aec8`:

| Commit | What |
|---|---|
| `bea2c2842` | merge `omamobile-acpm` (identical to the ACPM code in `omamobile-int`): DECON0 needs the ACPM clock provider |
| `04cbbeca6` | drm/panel: add an optional refresh rate switch (`drm_panel_set_refresh_rate()`, `funcs->set_refresh_rate`) |
| `32b4bf0c3` | drm/panel: samsung-s6e3hc4: switch the refresh rate through drm_panel (lists 60 and 120 Hz; the `refresh_rate` sysfs knob and `allow_rate_switch` are gone) |
| `445d2b4f7` | dt-bindings: DECON and DSIM get an OF graph `port` |
| `3e242dc3f` | drm: google: gs-dsim-cmd: switch the link and the panel between 60 and 120 Hz |
| `cb0400f56` | drm: google: gs-decon: add a 120 Hz mode and drive KMS by default |
| `5081097ac` | arm64: dts: gs201: DECON0 → DSIM0 port, ACPM `mif`/`disp` clocks on DECON0 |
| `d4b2e68a3` | arm64: dts: gs201-cheetah: enable DECON0 and its pool |

Merging into `omamobile-int` is conflict-free (`git merge-tree` checked).
Build in `out-display2`: release `7.3.0-rc6-omamobile-push1-00078-gd4b2e68a372c`,
logs `logs/{kernel,modules,initramfs,images}-display2-d5d.log`. W=1 is clean
for `gs-decon.o`, `gs-dsim-cmd.o`, `panel-samsung-s6e3hc4.o` and
`drm_panel.o`; checkpatch is clean; `dt_binding_check` passes for both
bindings; `dt-validate` of `gs201-cheetah.dtb` reports only the old `soc_id`
and `post-init-providers` items. That build is `omamobile-display2` alone
(no Wi-Fi, Bluetooth, …): for the phone, merge into `omamobile-int` and
build that.

**120 Hz switch (`gs-decon` + `gs-dsim-cmd` + panel).**

- The connector lists 1440x3120@60 and @120; 120 is preferred. A mode
  change between them needs no modeset: the CRTC check clears
  `mode_changed`, and the switch runs in the CRTC's `atomic_begin`, before the
  plane update of the same commit. So Hyprland's `preferred` rule gets 120 Hz,
  and `hyprctl monitors` lists both modes.
- Order, as D5.2 (and as gate D5c did by hand):
  - Up: `clk_set_min_rate(mif, 1352 MHz)` and `(disp, 310 MHz)`; wait for a
    DECON frame done (≤ 50 ms); DSIM `UNDERRUN_CTRL` = 0x3ec,
    `CMD_TE_CTRL1` = 0x19f51d93; panel rate set (ends with `F7 0F`, taken at
    the panel's next frame).
  - Down: frame done; panel; DSIM 0x1f09 / 0x33eb3b26; floors released.
  - A failed panel write is rolled back (old panel rate, old timers); the
    driver logs "staying at N Hz".
- The floors only raise a clock that runs below them. MIF runs at
  3172 MHz today, so they cost nothing now and protect a boot that leaves MIF
  low. Releasing a floor leaves the rate where it is. The 1352 MHz figure is
  stock's 120 Hz pairing.
- `gs-dsim-cmd` recognises the link from the boot values of those two
  registers (the 60 Hz pair, or the 120 Hz pair after a kexec) and refuses
  to switch a link it has no values for. `CMD_TE_CTRL0` (stable VFP) doesn't
  depend on the rate and stays as ABL left it, as in D5c.
- 120 Hz is offered only with: the DSIM on DECON0's port, a panel that
  implements `set_refresh_rate`, `gs_dsim_cmd.allow_tx=1` (set by
  `omamobile-device-cheetah`) and the `mif` clock. Otherwise only 60 Hz.
  `gs_decon.max_refresh=60` turns 120 Hz off.
- `/sys/bus/platform/devices/1c240000.display-controller/stats` gains
  `refresh_hz`.

**Default KMS and the boot handover.**

- `stage` defaults to 2 (KMS); 0 and 1 stay for debugging. `force_xrgb` is
  gone: DPP0 switches from ABL's ARGB8888 (code 0) to XRGB8888 (code 4) in
  the shadow update of the first DRM buffer, so the boot picture stays until
  then.
- `gs-decon` is a module, autoloaded by udev from the DT in Arch (about 4 s
  after boot, with `gs_dsim_cmd`, the panel and `clk_acpm`). It defers until
  the DSIM is bound and the panel has attached, and until `clk_acpm` provides
  the clocks. If any of them never comes, `gs-decon` never probes and
  **simpledrm keeps the screen as before**.
- **No fbdev console by default** (`gs_decon.fbdev=1` gives one). The chain
  is:
  1. ABL logo.
  2. The initramfs draws Omarchy's logo into simpledrm's `/dev/fb0`, which
     is the boot framebuffer.
  3. `gs-decon` probes and unplugs simpledrm. DPP0 keeps scanning the boot
     framebuffer, so the logo stays (fbcon loses its framebuffer and goes
     to the dummy console).
  4. Plymouth 26 ignores simpledrm until its `DeviceTimeout` (8 s) and takes
     a real KMS device as soon as one appears, so it opens `gs-decon`
     directly. It also drops a simpledrm renderer if it had already fallen
     back to one. Its first commit draws the same screen, at 120 Hz.
  5. `plymouth quit --retain-splash` (our drop-in) leaves its frame up,
     because no in-kernel client restores a console on last close, and
     Hyprland's first frame replaces it.

  With fbdev on, step 3 would turn black (fbcon takes over at once,
  `CONFIG_FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER` is off), and every
  last-close (Plymouth quit, session restart) would restore the black console.
  That was the brief black seen in D4a-2. The initramfs is unchanged: it
  carries no modules and still unmasks the DECON trigger (`TRIG_CON 0x3061`),
  which `gs-decon` needs.
- **Brightness** is the panel driver's backlight device
  (`/sys/class/backlight/1c2c0000.dsi.0`), registered on the DSI device. It
  is not tied to simpledrm, nor to `gs-decon` (which never calls
  `drm_panel_enable/disable`). `brightnessctl --class=backlight`, the
  quick-settings slider (logind) and the `bl_power` udev rule are unchanged.
  The rate switch and brightness writes are serialised by the panel's
  mutex.
- **DPMS** (`omarchy-brightness-display off`: Hyprland DPMS disable, then
  `bl_power` 4):
  - CRTC disable: DPP0 parks on the black boot framebuffer, the panel drops
    to 60 Hz (half the DPU memory traffic for the black frames), and the
    commit's event is sent at once.
  - Then `bl_power` 4 runs the panel's display-off set (D3a).
  - On: `bl_power` 0 runs display on, and the CRTC enable switches back to
    120 Hz before the first flip.
  - A custom commit tail updates planes only on an active CRTC and waits for
    `flip_done`, which the frame start interrupt or the 100 ms flip timeout
    always signals. So no commit waits for a vblank that a dark panel might
    not send.
  - **Missing:** DECON keeps running (no trigger masking) and the panel is
    never put to sleep (D3b/D4b: needs the full panel init and DSIM/DECON
    restart). "Off" is black frames to a display-off panel. Saving more
    needs D4a-3 (mask the trigger when idle) and D4b.
- **Session restart:** Hyprland's framebuffers are removed on exit, which
  turns the CRTC off (black, 60 Hz). The next session's first commit turns it
  back on at 120 Hz.
- **Shutdown, reboot, kexec:** `drm_atomic_helper_shutdown` turns the CRTC
  off, so the link and panel are back at 60 Hz on the black boot
  framebuffer.

**Not done: the panel's 10/30 Hz idle (auto mode, `BD 23`).** The panel
drops to its idle rate only when frames stop arriving. With the trigger
permanently unmasked, DECON sends a frame on every TE, so auto mode would
never idle. That needs D4a-3 (send frames only after a flip: mask the trigger
after each frame, TE interrupt for vblank) first. Then add auto mode with
the idle frame insertion as stock's `s6e3hc4_set_panel_feat()` does
(`B0 00 0C BD; BD 00 00; B0 00 12 BD; BD 00 00 16 00 06 01; BD 23` for
10 Hz).

### D6.2 Runbook (each step needs the user; one change per boot)

Before anything: `tools/phone-ssh dmesg -w` in a second terminal. Rollback
as in section 4 (panic=10, slot A fallback, known-good images). Every check
below is a read: no `devmem` writes are needed at any step (`devmem` on the
phone writes when given a second argument, so never pass one). Exception:
`display-hz --clear-underrun` (one write-one-to-clear of DSIM `INTSRC` bit 19).

**D6a: merge, build, install.**

1. In `~/android/mainline/wt-int`: `git merge omamobile-display2`; build
   `out-int` (`kernel`, `modules`, `initramfs`, `images`, with `KERNEL_DIR`
   and `KERNEL_OUT` set). Keep the current images in `known-good` first.
2. On the phone, remove the D4 test settings: the `gs_decon` line(s) in
   `/etc/modprobe.d/omamobile-decon.conf` (blacklist, `stage=2`,
   `force_xrgb=1`; the last one would now only log "unknown parameter").
   Remove `overlays/display-decon-test.dtso` from the boot DTB if it was
   applied; the base DTB enables DECON0 and the pool now.
3. `bin/omamobile modules-push && bin/omamobile push` (reboots into slot B).

**D6b: boot.** Watch the screen:
ABL logo → Omarchy logo → Plymouth animation → Hyprland, with no black frame
between the logo and Plymouth.

```sh
tools/phone-ssh 'dmesg | grep -E "gs-decon|gs-dsim|s6e3hc4|simpledrm|aperture"'
tools/phone-ssh 'ls /sys/class/drm; cat /sys/bus/platform/devices/1c240000.display-controller/stats'
tools/phone-ssh 'grep -wE "mif|disp" /sys/kernel/debug/clk/clk_summary'
```

- Expect:
  - "link timers for 60 Hz" (gs-dsim-cmd);
  - "flip-only KMS on DECON0, 1440x3120, up to 120 Hz (at 60 Hz)", then
    "120 Hz" once Plymouth commits;
  - no simpledrm card; `refresh_hz 120`; `flip_timeouts 0`;
  - MIF ≥ 1352 MHz, DISP ≥ 310 MHz.
- **Stop if:**
  - the screen goes black and stays black (check over SSH whether
    `gs-decon` is bound);
  - "staying at 60 Hz", a DSIM FIFO timeout, or "120 Hz needs …" in dmesg;
  - Plymouth doesn't show.

**D6c: 120 Hz under Hyprland.**

```sh
tools/phone-ssh 'sudo -u $(id -nu 1000) hyprctl -i 0 monitors'   # or on the phone
tools/display-hz --clear-underrun --secs 10                       # idle, scrolling, video
```

- Pass:
  - `1440x3120@120.00` with both modes in `availableModes`;
  - 119.5–120.5 frames/s, `UNDERRUN_CTRL` 0x3ec, underrun bit 0 after a
    minute of full-screen motion;
  - no flashing lines;
  - `flips` grows with `flip_timeouts` 0.
- **Stop if:** underrun bit set or flashing lines (go to rollback 1), or
  any flip timeouts.

**D6d: switch by mode.** In `~/.config/hypr/monitors.lua` set the mode to
`1440x3120@60`, save (Hyprland reloads), then back to `preferred`. Each
time: `display-hz` (≈60 then ≈120), `refresh_hz` in `stats`, no black
frame during the switch, and dmesg prints "60 Hz" / "120 Hz".

**D6e: brightness at 120 Hz.** The quick-settings slider and
`brightnessctl --class=backlight set 30%` / `100%`. Pass: the panel
follows, no FIFO timeout in dmesg (the LP window between frames is about
1.2 ms at 120 Hz).

**D6f: DPMS, 10 cycles.** Power key (or `omarchy-brightness-display off`,
wait 5 s, `on`).

- Pass:
  - the screen goes dark and comes back each time, with no hang;
  - `refresh_hz` reads 60 while off and 120 after;
  - `flip_timeouts` stays 0, apart from at most one per cycle if the dark
    panel sends no TE.
- **Stop if:** the screen doesn't come back. Recovery over SSH: `echo 0 >
  /sys/class/backlight/1c2c0000.dsi.0/bl_power` and `hyprctl dispatch dpms
  on`, else reboot.

**D6g: session restart, 3 times.** `systemctl restart sddm`. Pass: the
lock screen and Hyprland come back at 120 Hz, with at most a short black
frame while the old session exits.

**D6h: reboot twice and one kexec** (`bin/omamobile kexec`). Pass: same as
D6b. After the kexec, "link timers for 60 Hz" (shutdown went back to 60).

**Rollback, mildest first:**

1. 60 Hz only: `echo "options gs_decon max_refresh=60" >
   /etc/modprobe.d/omamobile-decon.conf`, reboot.
2. No KMS (simpledrm as before, brightness unaffected): `echo "blacklist
   gs_decon" > /etc/modprobe.d/omamobile-decon.conf`, reboot.
3. Console on the panel for debugging: `options gs_decon fbdev=1`.
4. Previous kernel: `push` the `known-good` images. The old kernel's
   `gs-decon` defaults to stage 0 (logs only), even with this DTB.

**Next:** D4a-3 (trigger masking, TE vblank), then the 10 Hz idle; D3b/D4b
(panel sleep, full DECON/DSIM restart) for real screen-off power savings.

### D6 result (2026-10-12, on the phone)

Kernel `omamobile-int` `…-00209-gd577cf9b38ce` (int with `omamobile-display2`
merged), default DTB, no decon overlay:

- `gs-decon` probed at 4.4 s. It logged "MIF at 2028000000 Hz", "DISP at
  400000000 Hz" and "flip-only KMS on DECON0, 1440x3120, up to 120 Hz (at
  60 Hz)", then "120 Hz" at 11.1 s.
- simpledrm is gone. `hyprctl monitors` shows 1440x3120@120.00, with modes
  120 and 60.
- `flip_timeouts` is 0. `display-hz` reads 120.01 Hz, `UNDERRUN_CTRL` 0x3ec,
  underrun bit 0.
- The boot splash, brightness and how it feels still need the user's check.
- One boot WARNING (`drm_plane.c:543`, the plane had no `destroy` hook) is
  fixed by `fe2647543`.

## D3b under KMS: stop the frames and sleep the panel with the screen off (2026-10-12, offline)

Today "screen off" means:

- DECON keeps sending a black frame on every TE (at 60 Hz since D6), so the
  DPU still reads 1.08 GB/s;
- the panel is only in display-off (`bl_power` 4), which still costs it
  about 87 mW.

Branch `omamobile-display2`:

| Commit | What |
|---|---|
| `dcc41a530` | panel: a taken-over panel is marked prepared/enabled. Backlight updates are held while it is in reset or asleep (`enable()` applies them). The rate switch refuses while asleep. Locks in prepare/enable/disable/unprepare. |
| `2ca20ad4d` | gs-dsim-cmd: `gs_dsim_panel_sleep()` (display off, sleep in `10`, reset), and `gs_dsim_panel_wake()` in two steps: the cold init (reset, DSC/PPS, sleep out, the init and feature set at 60 Hz, link timers back to 60 Hz), then display on with the backlight |
| `172c08f89` | gs-decon: `screen_off` module parameter (0644) |

The `screen_off` values:

- **0, the default:** black frames, as D6.
- **1:** after the black frame, mask DECON's hardware trigger (`TRIG_CON`
  bit 4). No frames are sent and no memory is read; DECON itself stays
  enabled.
- **2:** 1, then put the panel to sleep.

Turning the CRTC back on:

1. The panel's cold init runs.
2. The trigger is unmasked and one black frame goes out.
3. Display on and the backlight.
4. The CRTC's rate switch to 120 Hz.
5. The first flip.

Other behaviour:

- Remove and shutdown restore the boot state (panel lit, trigger running),
  so a kexec'd kernel finds it as ABL leaves it.
- `stats` shows `trigger_masked` and `panel_asleep`.
- A failed cold init leaves the panel in reset, and the next screen-on tries
  again.
- The default is unchanged, so the build is safe to merge before the test.

The wake is Google's order (reset, init, a frame, display on). It also
re-sends every register the init sets: VLIN1, VGH, VREG, black insertion,
toggle, SP, FFC, TSP sync, frame insertion, TE and rate. Sleep-out returns
those to the panel's defaults, and dark greys go wrong if they aren't
written again. What it does **not** do: restart the DSIM or DECON (D4b),
switch the panel's rails (no S2MPG12/13 regulator driver: they stay on, so
the panel sits in reset with power), or suspend (D4c).

Build: `out-display2`, release `7.3.0-rc6-omamobile-push1-00082-g172c08f89a2d`,
logs `logs/*-display2-d6.log`. W=1 and checkpatch are clean for the three
files, and it merges into `omamobile-int` without conflicts.

### D3b first try: DPMS off never reached the CRTC (2026-10-12)

With `screen_off=1`, Omarchy's DPMS off
(`hyprctl dispatch 'hl.dsp.dpms({ action = "disable" })'`) left the CRTC on:

- `trigger_masked` stayed 0, frame_start kept counting at about 120 a
  second, and `card2-DSI-1/dpms` read "On";
- Hyprland reported `dpmsStatus` false anyway.

**Cause: gs-decon refused the commit.**

- **What aquamarine 0.15 does:** it turns an output off with a real atomic
  commit (`src/backend/drm/impl/Atomic.cpp`). The connector's `CRTC_ID` goes
  to 0, the CRTC gets `ACTIVE` 0 (and `MODE_ID`), and the primary and cursor
  planes are detached (`FB_ID`/`CRTC_ID` 0). Neither the connector DPMS
  property nor anything else gs-decon lacks is involved.
- **Where it failed:** gs-decon's CRTC check called
  `drm_atomic_helper_check_crtc_primary_plane()` for every CRTC state,
  including one being turned off. With the plane detached it returned
  -EINVAL, so the commit failed and the CRTC kept scanning out.
- **Same bug elsewhere:** `drm_framebuffer_remove()`, when a client exits
  with its buffer on screen, failed the same way ("atomic remove_fb failed
  with -22" in dmesg). Session restarts recovered only because the next
  session's modeset took over.

**Fix:** `7d689978a` checks for the primary plane only on an active CRTC.
No fallback trigger (bl_power, idle vblanks) is needed: DPMS from Hyprland
and hypridle now reaches `atomic_disable`, and from there the black frame,
the drop to 60 Hz and `screen_off`. The CRTC turning off is the
upstream-standard signal, and a heuristic on the backlight or on flip idle
would fire on a static screen that is still on.

Build: `out-display2`, release `7.3.0-rc6-omamobile-push1-00083-g7d689978a20e`;
it merges into `omamobile-int` without conflicts.

### D3b runbook (each step needs the user)

Merge `omamobile-display2` into int, build, `modules-push`, `push`. Then
check that D6b/D6c still pass: `screen_off` is 0, so nothing else changed.

Run `tools/phone-ssh dmesg -w` in a second terminal.
`P=/sys/module/gs_decon/parameters/screen_off`,
`S=/sys/bus/platform/devices/1c240000.display-controller/stats`.
On the phone, `/tmp/hdpms disable|enable` runs Omarchy's
`hyprctl dispatch 'hl.dsp.dpms({ action = "disable" })'` (or `"enable"`) as
the user. `omarchy-brightness-display off/on` does the same plus `bl_power`
4/0.

**D3b-0: DPMS reaches the CRTC (screen_off 0).**

```sh
tools/phone-ssh '/tmp/hdpms disable'; sleep 3
tools/phone-ssh "cat $S; cat /sys/class/drm/card*-DSI-1/dpms"   # twice, 5 s apart
tools/phone-ssh '/tmp/hdpms enable'
```

- Pass while off:
  - the screen goes black (the parked frame);
  - `refresh_hz 60`, frame_start counting at about 60 a second;
  - `flips` stops growing;
  - no "atomic remove_fb failed" in dmesg.
- Pass after on: 120 Hz and the picture back.
- If it still doesn't switch off: `echo 0x10 >
  /sys/module/drm/parameters/debug` during one `hdpms disable` shows which
  check refuses (look for `[CRTC:` lines); then `echo 0` to turn it off.

**D3b-1: stop frames while off.**

```sh
tools/phone-ssh "echo 1 > $P"
tools/phone-ssh '/tmp/hdpms disable'; sleep 5
tools/phone-ssh "cat $S; grep gs-decon /proc/interrupts"   # twice, 5 s apart
tools/phone-ssh '/tmp/hdpms enable'
```

- Pass while off: `trigger_masked 1`; the frame_start count doesn't move;
  no TE timeout or FIFO errors in dmesg.
- Pass after on: the screen comes back at once at 120 Hz; `trigger_masked 0`;
  `flip_timeouts` unchanged; brightness works.
- Repeat 10 times, then 5 times with the power key (Omarchy's full path,
  including `bl_power`).
- **Stop if** the screen stays black after enable. Recovery: `echo 0 > $P`,
  then `/tmp/hdpms disable; /tmp/hdpms enable`, else reboot.

**D3b-1 result (2026-10-12, `…-00249-g678ee4065295`):** cycle 1 passed.
Cycles 2 and 3 failed: `hdpms disable` returned "ok" and Hyprland reported
DPMS off, but no commit reached the CRTC (connector "On", about 121 frames a
second). The Hyprland log had "drm: Cannot commit when a page-flip is
awaiting" (aquamarine) once per failed off. In cycle 1 the screen was idle;
in cycles 2 and 3 a client kept rendering (85 flips in 3 s).

**Analysis (offline, Hyprland v0.56.2 and aquamarine 0.15.0 sources).**

- Hyprland's DPMS off (`CMonitor::setDPMS(false)`) first fades to black.
  At the end of the fade it calls `commitDPMSState(false)`
  (`setEnabled(false)` and a commit).
- If that commit is refused, Hyprland logs "Couldn't commit output … for
  DPMS = 0, will retry", retries once 2 frames later (16.7 ms at 120 Hz),
  then gives up with "Couldn't retry committing output". `dpmsStatus` stays
  false and the output stays on. It never tries again.
- Aquamarine refuses output commits while a page flip is busy:
  - `CDRMOutput::commit()` returns false (without a log line) while an
    asynchronous commit is queued or in flight and can't be cancelled;
  - a commit that carries a buffer while a flip is awaiting is refused
    with the logged "Cannot commit when a page-flip is awaiting".
- A client that keeps rendering keeps a flip pending most of the time, so
  both tries can land in it. That is generic Hyprland/aquamarine behaviour,
  not specific to gs-decon. Desktops rarely hit it because DPMS usually
  comes when the screen is idle.
- How long our flips stay pending matters, though:
  - gs-decon completed a flip at the first frame start whose interrupt
    already saw the shadow update request cleared;
  - DECON clears it as the frame starts, so an interrupt that read it a
    moment early would delay the event by a whole frame;
  - typical drivers send the event at the vblank that latched the update.
- **The wake path (D3b-1 cycle 1) loses no event:**
  - the DPMS-off commit only starts after the previous flip's commit is
    done (DRM stalls a blocking commit on the previous commit's cleanup,
    which waits for its flip_done);
  - the park's own shadow update has no event and is waited for before the
    trigger is masked;
  - the off commit's own event is sent in `atomic_disable`;
  - on enable, the trigger is unmasked before the plane update, and the
    flip is armed in `atomic_flush` and completed by the next frame start,
    or by the 100 ms timeout if no TE comes.

**Changes:**

- Kernel `5f4808c32` (gs-decon):
  - at frame start, poll the update request for up to 20 us before
    deferring the flip, and check again at frame done;
  - stats gain `flips_late` (flips completed at frame done) and
    `flip_latency_us_avg/max` (from arming the event to sending it).
  - At 120 Hz, expect an average of half a frame (≈ 4 ms) and a maximum
    of about 8.4 ms. A maximum near 16.7 ms, or `flips_late` growing, means
    DECON clears the request later than assumed.
- Hyprland patch `packages/hyprland/0001-dpms-keep-retrying-a-refused-dpms-commit.patch` (packaged in `packages/hyprland`)
  (against v0.56.2, Omarchy-patch style):
  - keep retrying the refused DPMS commit every 2 frames for up to a
    second instead of once;
  - damage the monitor after a retried DPMS-on.
  - Packaged as `hyprland`/`hyprpm` 0.56.2-5.1 (Omarchy's 0.56.2-5 plus
    this patch). It compiled in a full x86_64 test build. Phone build,
    install and rollback: `packages/hyprland/README.md`.
- No driver-side fallback (backlight or idle triggers): with the retry the
  real commit gets through, and the driver can't tell "off" from "static"
  without it.

Build: `out-display2`, release `7.3.0-rc6-omamobile-push1-00084-g5f4808c32c52`;
it merges into `omamobile-int` without conflicts.

**D3b-1b: retest with the new kernel (and, once built, the patched
Hyprland).** Use `screen_off=1`. Keep an app animating (a video or a
spinner) and run 10 cycles:

```sh
tools/phone-ssh "echo 1 > $P"
tools/phone-ssh '/tmp/hdpms disable'; sleep 3
tools/phone-ssh "cat $S; cat /sys/class/drm/card*-DSI-1/dpms"
tools/phone-ssh '/tmp/hdpms enable'; sleep 2
tools/phone-ssh "grep -E 'for DPMS|page-flip' \$XDG_RUNTIME_DIR/hypr/*/hyprland.log | tail"   # as the user
```

- Read `flip_latency_us_avg/max` and `flips_late` with the screen on and
  the app animating, before the cycles.
- Pass with the patched Hyprland: every cycle reaches `trigger_masked 1`
  and dpms "Off". The log may show "will retry" but never "Couldn't retry
  committing output".
- With the stock Hyprland and only the new kernel, count how many of the
  10 cycles fail. The log lines say which aquamarine check refused:
  - "will retry" followed by "Couldn't retry …": the busy-flip window
    (expected);
  - no "for DPMS" line at all: the fade's end callback never ran. Report
    that, since it's a different bug.
- **Stop if** the screen doesn't come back on enable (as in D3b-1).

**D3b-1b result (2026-10-12):** kernel `…-00253-g48b42df9b5c7` (has
`5f4808c32`), hyprland 0.56.2-5.1 built on the phone (about 20 min at -j4),
`screen_off=1`, 6 cycles of `/tmp/hdpms disable`, 3 s, `enable`.

- **Cycles 2–6 pass:** dpms "Off", `trigger_masked 1`, 0 frames a second
  while off; 120 Hz and 120 frames a second after on.
- **Cycle 1 fails:** dpms "On", 121 frames a second. The session had just
  restarted, and the PIN was probably being typed on the lock screen.
- **Stats:** `flip_timeouts 0`, `flips_late 0`, `flip_latency_us_avg 4124`,
  `flip_latency_us_max 9669`. That is half a frame on average, and never
  more than one frame plus scheduling at 120 Hz: flips complete at the frame
  that latched them.
- **Hyprland log:** no "for DPMS" lines, and two "Cannot commit when a
  page-flip is awaiting".

**Reading of the result (offline, Hyprland v0.56.2 source).**

- **The log can't show DPMS retries as configured.** `debug:disable_logs`
  defaults to true. Once the config is loaded, `CLogger` drops every
  message, whatever its level (`src/debug/log/Logger.cpp`), including the
  patch's error line and Hyprland's own "will retry". The two aquamarine
  lines are from before the config loaded at session start. So "0 lines"
  says nothing about cycle 1.
- **Cycle 1 most likely wasn't a refused commit at all.** Omarchy sets
  `misc:key_press_enables_dpms = true` and `mouse_move_enables_dpms = true`
  (`default/hypr/input.lua`). Any key press re-enables DPMS
  (`InputManager.cpp`, `Config::Actions::dpms(ENABLE)`), and that includes
  the on-screen keyboard typing the PIN, which is a virtual keyboard. So
  typing on the lock screen within those 3 s turns the screen back on: dpms
  "On", 120 Hz. That is Omarchy's intended behaviour, not a bug. D3b-1c
  checks it.
- **Is a 1 s retry enough while something animates (the lock screen)?**
  - Aquamarine refuses only while an asynchronous commit is queued or in
    flight. With our flips that is at most about 9.7 ms, 4.1 ms on average,
    so about half the time at a steady 120 fps.
  - The retry timer runs every `2000/refresh` ms (16 ms at 120 Hz), which
    drifts against the 8.33 ms frame. The tries don't phase-lock onto the
    busy part.
  - About 60 tries at a roughly 50% chance each fail together with
    negligible probability.
  - Retrying from the output's present event would be tidier. But Hyprland
    renders the next frame from the same event, so the DPMS commit would
    race the next frame's commit in listener order. It is a bigger and less
    obvious change, and not needed unless D3b-1c shows "Couldn't retry
    committing output".

**D3b-1c: confirm, with logs on.** As the user on the phone (or over SSH
with the session's `HYPRLAND_INSTANCE_SIGNATURE` and `XDG_RUNTIME_DIR`):

```sh
hyprctl eval 'hl.config({ debug = { disable_logs = false } })'
hyprctl getoption debug:disable_logs          # expect false
L=$(ls -d $XDG_RUNTIME_DIR/hypr/*/ | tail -1)hyprland.log
```

1. **Hands off, with something animating.** For example
   `alacritty -e top -d 0.1` in front, or the lock screen's clock. Run 10
   cycles of `/tmp/hdpms disable`, 3 s, read `$S` and the connector's
   `dpms`, `/tmp/hdpms enable`, 2 s. Don't touch the screen or type during
   the cycles.
2. `grep -nE 'for DPMS|page-flip|dpms' $L | tail -40`
3. **Wake on a key.** Run `/tmp/hdpms disable`, then press a volume key (or
   type on the on-screen keyboard) within 3 s. Expect dpms "On" and 120
   frames a second. That confirms the wake-on-key reading of cycle 1.

- **Pass:** all 10 hands-off cycles reach `trigger_masked 1` and dpms "Off".
  Any "will retry" line is followed by success, never by "Couldn't retry
  committing output". Step 3 shows the key wake.
- **If** "Couldn't retry committing output" appears: send the log section.
  The next step is then a retry tied to the output's present event.
- Turn logging off again afterwards
  (`hyprctl eval 'hl.config({ debug = { disable_logs = true } })'`): it
  writes on every frame.
- Leave `screen_off=1`, then go on to D3b-2.

**D3b-2: panel sleep.** Only after D3b-1c passes. Hands off during each
cycle, as in D3b-1c: a key press wakes the screen.

```sh
tools/phone-ssh "echo 2 > $P"
tools/phone-ssh '/tmp/hdpms disable'; sleep 10
tools/phone-ssh "cat $S"            # panel_asleep 1, trigger_masked 1
tools/phone-ssh '/tmp/hdpms enable'; sleep 2
tools/phone-ssh "cat $S; dmesg | grep -E 'gs-decon|gs-dsim|s6e3hc4' | tail"
```

- **Pass:**
  - the screen comes back within about half a second, with correct colours.
    Dark greys must stay grey, not black: compare the wallpaper and a dark
    theme with a screenshot taken before;
  - `display-hz` reads 120 and `refresh_hz` is 120;
  - brightness follows the slider;
  - `panel_asleep 0`, `trigger_masked 0`, `flip_timeouts` unchanged;
  - dmesg shows no "cold init failed", "display on failed", DSIM FIFO
    timeout or "staying at 60 Hz".
- Do 5 cycles with hdpms. Then do 5 with the power key, which also runs
  `bl_power` 4/0 around them; the panel driver holds those while the panel
  sleeps. Then 2 cycles where a key press does the wake.
- **Stop if:**
  - the panel stays dark. Do one more disable/enable, since the cold init
    retries at the next screen-on; then `echo 0 > $P` and another cycle;
    else reboot, and ABL re-initialises the panel;
  - the colours are wrong after wake (a panel register the init doesn't
    write; take a photo and compare with D6);
  - garbage shows before the picture (display on came before the black
    frame);
  - a flip timeout or a hang on enable.

**D3b-3: power.** Measure `current_now` of the fuel gauge with the screen
off for two minutes at each `screen_off` value 0/1/2 (USB unplugged,
over Wi-Fi). Expect 1 to save the DPU and memory traffic, and 2 another
≈ 80–90 mW.

**D3b-4: soak.** With `screen_off=2`:

- 20 power-key cycles, including some within 1 s of each other;
- an idle-lock and screen-off by hypridle;
- `systemctl restart sddm` with the screen off and with it on;
- one reboot and one kexec with the screen off.

Pass: every wake works, and after a reboot or kexec "link timers for 60 Hz"
appears and the boot shows the splash.

**Then make it the default:** `options gs_decon screen_off=2` in
`omamobile-device-cheetah`'s `modprobe.d/omamobile-display.conf`, or change
the driver default.

**Rollback:** `echo 0 > $P` at run time (takes effect at the next screen
off); `options gs_decon screen_off=0`; or the D6 rollback list.

**Next:** D4a-3 (keep the trigger masked between flips, vblank from DECON's
TE interrupt), and then the panel's 10 Hz idle at 120 Hz. D4b (DSIM/DECON
restart) only if the panel wake proves unreliable without it.
