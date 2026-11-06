# Findings

Everything measured so far on the target machine. Numbers here are facts;
interpretation lives in [DESIGN.md](DESIGN.md).

## Machine

| | |
|---|---|
| Laptop | HP EliteBook 840 G8 (Tiger Lake i7, Iris Xe `8086:9a49`), HP platform ID **T37** |
| BIOS | T37 Ver. 01.19.00 (11/19/2024). Latest published: **01.24.02** |
| Battery | ~3.45 Ah @ 12.4 V ≈ **43 Wh** (estimated from `charge_now` at 99 %) |
| macOS | Tahoe 26.7.1 (also Sequoia, Ventura), OpenCore 1.0.8, SMBIOS `MacBookPro16,2` |
| EFI repo | `labrouss/EliteBook840G8` — see its `docs/SLEEP.md` |

## 1. ACPI S3 resume is broken in firmware

- Entering S3 works; **resume hangs** (black screen, needs a hard power-off).
- The **same hang happens on Linux** with `mem_sleep=deep`, so it is not a
  macOS/kext problem. `pm_trace` was used during that investigation.
- HP's BIOS release notes from 01.10.00 to 01.24.02 never mention S3, but
  **01.12.00 says "Improves modern standby"**: Windows uses Modern Standby on
  this machine and the S3 path is legacy/untested.
- Current macOS workaround: `ACPI > Patch` turns `_S3` SLP_TYP 5 → 7
  (Find `5F53335F1207040A05`, Replace `5F53335F1207040A07`), so sleep is a
  clean power-off.

## 2. Hibernation (S4-style, macOS `hibernatemode 25`) — first attempt failed

- Caused **BIOS "Real-Time Clock power loss (005)"** CMOS resets, and the
  restore hung.
- The **normal-shutdown** 005 is fixed: AppleRTC was writing into HP's
  CMOS. RTCMemoryFixup 1.0.7 with `rtcfx_exclude=80-AB` plus OpenCore
  `DisableRtcChecksum` stop it.
- The **restore** 005 is **not** fixed: per the EFI repo's `docs/SLEEP.md`,
  boot.efi writes the hibernation key into RTC 0x80–0xAB during restore,
  before macOS (and RTCMemoryFixup) runs. That attempt already used
  `HibernateMode` Auto and NVRAM. See HIBERNATION-FALLBACK.md for the next
  try (`AppleRtcRam` + `rtc-blacklist`).
- The hung restore has not been retried.

## 3. Modern Standby (s2idle / S0ix) works perfectly on Linux

Ubuntu 26.04.1 live USB, kernel 7.0.0-30-generic, BIOS 01.19.00.

- `/sys/power/mem_sleep` = `[s2idle] deep`: **s2idle is the firmware default**.
- Test 1 (short): SLP_S0 residency 0 → 19,255,992 µs. The machine **resumed
  cleanly** (live session intact; only the SSH session dropped with Wi-Fi).
- Test 2 (`linux/s2idle-test.sh`, on battery, 2026-11-02):

| Metric | Result |
|---|---|
| Elapsed | 10.8 min (645 s) |
| Battery used | 11.0 mAh = 0.14 Wh |
| **Average power** | **0.76 W** (≈ 0.6–0.9 W given the coarse counter) |
| **S0ix residency** | **634.4 s of 645 s = 98 %** |
| Projected standby | ≈ 1.8 %/hour, ≈ 2 days from full |

**Conclusion:** hardware and firmware fully support S0ix with a clean wake.
The only missing piece for real sleep on macOS is software.

## 4. Baseline capture (2026-11-02, `s2idle-baseline-20261102-093936`)

| Item | Value | Why it matters |
|---|---|---|
| FADT | `Low Power S0 Idle = 1`, `Control Method Power Button = 0` | S0ix declared; power button is the **fixed-feature** button (PM1 status, SCI) |
| Wake on Linux | `ACPI: PM: ACPI fixed event wakeup` via SCI (IRQ 9) | Power-button wake needs only the SCI: answers part of Q11 |
| Substates | Only **S0i3.0** used (S0i2.0 = 0); +29.16 s in the capture suspend | Target state is S0i3 |
| LPIT | Native C-state entry `FFH 0x60` (MWAIT C10), residency counter **MMIO `0xFE00193C`** (32-bit), **8197 Hz** (≈ 122 µs/tick); min. residency 30 ms, latency 3 ms | The phase 0 meter reads this generically from LPIT: no hardcoded PWRMBASE |
| LPS0 device | `\_SB.PEPD` (`INT33A1`, `_CID PNP0D80`) in SSDT14 `MS-SSDT` | — |
| `_DSM` GUIDs | Intel `c4eb40a0-…` mask `0x7F` (fn 0–6); `57a6512e-…` (fn 1, L0IX buffer). **No Microsoft `11e00d56-…`** | — |
| fn 3 / 4 (display off / on) | `ECNT(1)` / `ECNT(0)`: EC `SLPT = 6` / `0`, GPIO `0x090B000E`, `ECUP` | **EC sleep signalling: Deep Idle without `_DSM` leaves the EC awake** |
| fn 5 / 6 (entry / exit) | `GUAM(1)` / `GUAM(0)`, `ECNT(3/2)`, `DPOF` for TCSS xHCI | Platform "user absent" mode + TCSS |
| Name conflicts | No `LPS0`, `LXEN`, `_TTS` anywhere in DSDT/SSDTs | `SSDT-DEEPIDLE` is safe to add |
| ACPI root | Host bridge is **`\_SB.PC00`**, not `PCI0` | Any new SSDT must use `PC00` paths |

The capture script now also saves `substate_requirements` and
`substate_live_status_registers` (missed in this run). Re-run it after the
BIOS update anyway.

`s0ix-selftest-tool.sh` did not run usefully: `-s` could not run its
bundled `turbostat`, and `-r off` was started over SSH (`localhost:10.0`,
X forwarding). See TESTING.md.

## 5. macOS Deep Idle on this HP (2026-11-02)

| Item | Result |
|---|---|
| `SSDT-DEEPIDLE` | macOS sleeps with type 7 (Deep Idle) and wakes on the power button (`PWRB`) |
| **CPUID spoof** | With `Cpuid1Data` = Coffee Lake (`0x906EA`) XCPM never goes below **core C6 / package PC6** in Deep Idle (`machdep.xcpm.deep_idle_last_stats`: CC7 0 %, C6 95 %). With **Ice Lake (`0x706E5`)**: **CC7 99 %, package C10 99 %** |
| Why | XCPM picks idle states from the table of the CPU model it is told. Tiger Lake reports `CPUID.5:EDX` = `0x11121020` (no MWAIT C3 hint, fewer sub-states), identical to Ice Lake, unlike Coffee Lake. **Tiger Lake hackintoshes must spoof Ice Lake for deep idle** |
| MSR 0xE2 | `0x74008008`: package limit C10, CFG lock on (not a limit) |
| LPS0 `_DSM` fn 5/6 | Safe: wakes normally |
| LPS0 `_DSM` fn 3/4 | Puts the EC into its standby mode (power LED breathes); with the Coffee Lake spoof the wake never reached the CPU (`Failure during sleep 0x1F`) |
| SLP_S0 residency | 0 so far, even at PC10 99 % (fn 3/4 not yet tested with the Ice Lake spoof) |
| PMC page at the halt (`-s0ixpmcdump`) | Readable (SLP_S0 counter at `0xFE00193C` really is 0). Power-gating register `PWRM+0x1D90` (1 = gated): at the macOS halt every PCH IP matches Linux **except #17 ISH, still powered**. xHCI is gated (USB stick not a blocker). macOS has no ISH driver; on Linux `intel_ish_ipc` puts it in its low-power state |
| WhateverXe | Acceleration panics on any wake (`context-ID-mismatch`); `-wex-noaccel` avoids it. Upstream: sleep unsupported |

## 6. First usable result (2026-11-02)

With `SSDT-DEEPIDLE`, S0ixStandby 0.1.10 (all `_DSM` functions, `-s0ixd3`),
Ice Lake CPUID spoof and WhateverXe `-wex-noaccel`: **0.96 W over 21 min**
(Linux s2idle 0.76 W), package PC10 99 %, EC in standby, wake on the power
button and on RTC alarms. SLP_S0 still 0: **ISH** and **CNVI** stay powered.
Neither powers down in D3hot (CNVi not even with the Wi-Fi/BT kexts
disabled); both need their driver's firmware handshake (Linux:
`intel_ish_ipc`, `iwlwifi`/`btusb`).

Overnight (2026-11-02/03, 11 h 27 min, 0.1.12): **26 RTC/Maintenance dark
wakes, every one back to sleep**, clean power-button wake in the morning,
battery 100 % → 80 % (≈ 0.7–1.0 W average, dark wakes included).

Open risk: one earlier ~1 h sleep hung at **DarkWakeExit** (`0x32`): the
switch from a dark wake to full wake never finished. Not reproduced in 26 dark
wakes; most likely only when the power button is pressed *during* a
(seconds-long) dark wake. Suspect the graphics path (WhateverXe).

## 7. AirportItlwm sleep path (laobamac/itlwm, 2026-11-01 HEAD)

`setPowerState(Off)` → `setPowerStateOff()` → `disableAdapter()` →
`ItlIwx::disable()` → `iwx_activate(DVACT_QUIESCE)` → `iwx_stop()`: DMA stop,
`iwx_apm_stop()` (low-power state), SW reset, RF-kill interrupt re-armed;
the same sequence as OpenBSD/Linux. **The PCI function itself is never put in
D3hot** (`IOPCIEDeviceWrapper::setPowerState` only acks). Linux's PCI core
does that after the driver's suspend. Test: S0ixStandby `-s0ixd3net`.
**Bluetooth** (lshbluesky/IntelBluetoothFirmware, 2026-10-18 HEAD): the kext
is a firmware loader only. `start()` uploads the firmware and closes the
device again, and `setPowerState()` only acks. Afterwards Apple's own
Bluetooth stack drives the controller (IntelBTPatcher patches Apple's USB
transport), so BT sleep is Apple's USB suspend; xHCI is gated at the halt.
No BT sleep code to fix there.

**CNVi status: parked.** It stays powered in every macOS combination tried
(drivers on, radios off, no drivers, Wi-Fi function in D3hot). The likely
missing piece is a firmware-level D3 handshake (Wi-Fi and/or BT), on the
driver side, worth about 0.2 W (0.96 W vs Linux 0.76 W).

## 8. Ruling out blockers (2026-11-02)

| Candidate | Test | Result |
|---|---|---|
| `XSQD` (PWRM+0x1B1C bit 22, set by ACPI `S023` votes of HDA/display/CNVi Wi-Fi/CNVi BT) | Linux reads the same `0x0140801e` (bit set) and reaches S0i3 | **Not a blocker** |
| **ISH** (no macOS driver) | Linux s2idle with `intel_ish_ipc` never loaded: 96 % S0ix | **Not a blocker** (now ignored by the blocker report) |
| **CNVi** (Wi-Fi + BT) | Powered at every macOS halt: with AirportItlwm/IntelBTUnified, with radios off, without the kexts, and with the Wi-Fi function in D3hot (`-s0ixd3net`, which also broke Wi-Fi resume) | **Only remaining candidate**; needs the drivers' suspend handling (Wi-Fi and/or BT) |

`substate_requirements` is empty on this firmware (no per-IP requirement table).
`CNVW._PS3` / `HDAS._PS3` only feed the `S023` vote (`XSQD`), so running
them would not help.

## 9. WhateverXe acceleration across Deep Idle (2026-11-05)

Graphics stack: WhateverXe.kext (Lilu plugin, EFI) plus the WhateverXe
Graphics package (`AppleIntelTGLGraphics` 16.0.32, Apple's pre-release Tiger
Lake driver from a Big Sur beta, installed under `/Library`).

| Run | Result |
|---|---|
| WhateverXe 0.6.1, acceleration on | panic ~1 s after wake: `r113 context-ID-mismatch` (Apple's CSB validator) |
| WhateverXe 0.7.0, acceleration on | before sleep healthy (4 engines, 0 CSB errors, `GFX_MODE` 0x8 = execlists on); **panic 6.0 s after wake**: `r113 ElementSwitch-second-item-null` from the render engine's context-switch interrupt. Black screen and no ping = the panic, not a hang |
| `-wex-noaccel` | sleeps and wakes cleanly (all tests above) |

What the driver does (disassembly of the Tahoe payload, symbols present):

- Sleep: `IntelAccelerator::setPowerState(off)` → `IGStolenMemoryPool::purge`,
  `IGInterruptBridge::systemWillSleep`, `stopGraphicsEngine`.
- Wake: `setPowerState(on)` → `IGInterruptBridge::systemDidWake`,
  **`startGraphicsEngine`** (the boot path: `initModeRegisters`,
  `initHardwareStatusPageRegisters`, `initFirmware`, interrupts, default
  context); `IGScheduler5::systemDidWake` → `resetOnWake` per engine.
- `IGHardwareCommandStreamer5::resetOnWake` only resets the CSB read pointer
  (to 7, a 6-entry layout). WhateverXe replaces it with a 12-entry version on
  purpose; calling Apple's original as well would be wrong.
- `lastHWS = ffff…` in the panics is WhateverXe's consumed-slot sentinel, not
  a dead device.

Reading: the GPU reported an element switch (port 0 done, port 1 running)
while the driver had only one item queued. **GPU and driver queue state
disagree after wake.** Apple's wake path assumes S3, where the GPU loses power
and starts blank. In Deep Idle the iGPU likely keeps its power and state; Linux
resets the GT on resume for the same reason.

Test: S0ixStandby 0.3.0 `-s0ixgpuprobe` (does the GPU keep its state?) and
`-s0ixgpureset` (full GT reset at wake, before the driver restarts the engines).

Result with `-wex-noaccel` (0.3.1, 3.3 min): **the GT loses its engine
state** (canary in render GPR0 gone), while the PCI function keeps its
config (same BAR0, D0; only memory/IO/bus-master bits cleared until macOS's
PCI layer restores them). So without acceleration the GPU does wake blank,
as Apple's driver assumes. Open: the same with acceleration on (the driver
enables RC6, whose hardware save/restore may behave differently); 0.3.2
keeps the probe result in NVRAM to survive the panic. If it is blank there
too, the panic is software state (WhateverXe/Apple execlist bookkeeping not
cleared on wake), to be fixed in WhateverXe.

Result with acceleration on (0.3.4, NVRAM record survived the panic):
**`retained`**: status page address `0x40004000` and `GFX_MODE` 0x8
(execlists) identical at sleep and at the wake action, i.e. before Apple's
driver restarts the engines. With the accelerator running (RC6 enabled) the
GT keeps its state through Deep Idle, and Apple's S3-style restart lands on
a GPU that is not blank. Next: `-s0ixgpureset` with acceleration.

**Fix confirmed (0.3.4, `-s0ixgpureset`, acceleration on):** at the wake
action the GT is reset (GDRST full, 54–94 us, render + blitter ready), so
Apple's `startGraphicsEngine` finds the blank GPU it was written for.
WhateverXe reads `GFX_MODE` 0 at its wake init (after the reset), Apple then
re-enables execlists. First accelerated wakes ever: 3/3 short sleeps clean,
Metal 2 after wake, no CSB errors; then a 40 min sleep, clean, video
playback after wake. Default since 0.4.0. 4 h 8 min idle sleep on battery
with 5 dark wakes: 6/6 resets (46–94 us), clean, ~0.5 W.

## 10. CNVi: why AirportItlwm leaves Wi-Fi in D0 (2026-11-05)

AirportItlwm's sleep path (`setPowerStateOff` → `disableAdapter` → `iwx_stop`
→ `iwx_stop_device`) quiesces the chip the way Linux iwlwifi does: interrupts
off, DMA stopped, `MAC_ACCESS_REQ` released, APM stopped, `INIT_DONE` cleared,
SW reset. What it never does is let the PCI function leave D0: it does not
call `IOPCIDevice::enablePCIPowerManagement()` (Apple's drivers do; IOPCIFamily
then writes D3 in `setPCIPowerState`, evaluates the device's ACPI `_PS3`, and
restores config space on wake), it pins the domain on with
`requestPowerDomainState(kIOPMPowerOn, …)`, and its PCI wrapper's
`setPowerState` is a no-op. Linux puts the function into D3hot after the
driver's suspend and runs `CNVW._PS3`.

0.1.11 `-s0ixd3net` wrote PMCSR directly: config space (BARs, MSI) was not
restored on D3 → D0 and Wi-Fi never came back. 0.4.1 `-s0ixwifipm` asks
IOPCIFamily instead: Wi-Fi in D3 for the whole sleep, back by itself after
wake, but **CNVI unchanged**, also with Bluetooth off.

Linux (2026-11-05, 1 min s2idle): `module_blacklist=btusb,btintel` **85 %**,
`module_blacklist=iwlwifi` **89 %** S0ix. So Linux does not need either CNVi
driver, and CNVI shows `State: On` in Linux's power-gating list too: the
CNVi power-gating bit is not what keeps SLP_S0 at 0. Next: the PMC
requirement status registers (`LPM_STATUS_0..5`, Linux
`substate_status_registers`) at the macOS halt vs Linux at S0ix entry
(`macos/lpm-status.sh`, needs `-s0ixpmcdump`).

First result (0.4.1, `-s0ixwifipm`): the six LPM status registers are at
`PWRM+0x1C5C..0x1C70` on this PCH (not Linux's TGL offsets; found by matching
Linux's values). At the macOS halt, differing from Linux at S0ix entry:
**`CNVI_REQ` = 1** (Linux 0; macOS awake at boot: 0), `USB2_SUS_PG_Sys_REQ`
and `MPHY_Core_DL_REQ` = 1, `PSF`/`SBR` fabric not gated (may still change
after the halt snapshot). **Correction (Linux `/dev/mem` dumps):** those six registers are the
**live** status; the **latched** set (state at package C10 entry, Linux's
`substate_status_registers`) is at `PWRM+0x1C3C..0x1C50`, all zero in the
macOS snapshots (boot and before the halt). Awake, live vs live, macOS and
Linux nearly match (`CNVI_REQ` 0 on both; macOS keeps the USB2 PLL on, ADSP
not in D3, no ISH driver). For these bits 1 most likely means "requirement
met" (`CPU_C10_REQ` 0 awake, 1 in C10), so `CNVI_REQ` 1 before the halt is not
a blocker; the bits still 0 there (`CPU_C10_REQ`, bits 13–15, PSF/SBR
gating) only change after the CPU halts, on Linux too. The A–D1 tests
(Bluetooth/Wi-Fi off, no `-s0ixwifipm`, `s0ixfn=0x18`) all gave identical
pre-halt values for that reason. 0.4.2 adds a snapshot right after the halt
to read the latched set. Open: Bluetooth (same CNVi; a connected BT device stayed
connected through sleep, and CNVI stayed powered even without the Wi-Fi/BT
kexts), so the test is Wi-Fi D3 alone, then with Bluetooth off.

## 11. The iGPU display handshake is what unlocks SLP_S0 (2026-11-06)

Full 32-bit Linux and macOS PMC page dumps (`PWRM+0x1000`) differ in one
block of settings that never changes on Linux: `PWRM+0x1CAC` = 0x9 (same mask
as `LPM_EN`), `0x1CB0` = 0xFF, `0x1CB4` = 0xFF (0x1CB8 = 0x80 only with
`intel_pmc_core` loaded). macOS: all 0, at kext start and before the halt.
Neither Linux's PMC driver (mainline `drivers/platform/x86/intel/pmc`) nor
the ACPI tables name or write these offsets, and the CPU cannot write them
(an OpenShell `mm` write reads back 0): the PMC sets them itself.

Linux bisection (read with `/dev/mem`):

| Linux boot | 0x1CAC / 0x1CB0 / 0x1CB4 | SLP_S0 counter across a suspend |
|---|---|---|
| OpenShell, before any OS | 0 / 0 / 0 | – |
| `break=top` (initramfs, before drivers) | 0 / 0 / 0 | – |
| normal | 9 / FF / FF | rises |
| `module_blacklist=intel_pmc_core` | 9 / FF / FF | rises (0x2E417 → 0x79100) |
| `module_blacklist=i915,mei_me,snd_sof_pci_intel_tgl,snd_hda_intel,intel_vsec,thunderbolt` | 0 / 0 / 0 | – |
| `module_blacklist=i915` | **0 / 0 / 0** | **stays 0** |
| macOS (XOSI on, `PEPD._STA` = 15) | 0 / 0 / 0 | stays 0 |

**Linux without i915 behaves exactly like macOS: no SLP_S0 at all.** The
remaining S0ix blocker on this machine is what the graphics driver does for
the display/GT power handshake, not Wi-Fi, Bluetooth, CNVi or ISH. OSYS is
ruled out (XOSI answers up to Windows 2021, PEPD present).

Linux reference with i915 (`i915_dmc_info`, normal boot): DMC `tgl_dmc_ver2_12.bin`
2.12 loaded (main and pipe A), program base 0x0c004040, ssp base 0x00086fc0,
htp 0x01a40188, DC3→DC5 95, DC5→DC6 793, DC3CO 0.

`i915.enable_dc=0` (DMC still loaded): the block reads 0 and DC3→DC5 /
DC5→DC6 stay 0, so **the block records the display reaching DC5/DC6 while
awake**. Yet the SLP_S0 counter still rose across a suspend (0 → 0x121A9D):
**DC5/DC6 at runtime is not required for S0ix; what i915 does on suspend is.**
On s2idle, display version 11+, i915 (mainline `intel_display_power.c`,
`intel_display_power_suspend_late`) does:

1. `icl_display_core_uninit`: DC states off, DMC program disabled, DBUF off,
   CDCLK off, power well 1 off, combo PHY uninit (skipped only when DC9 is
   not allowed and the DMC stays in charge).
2. `bxt_enable_dc9`: `DC_STATE_EN` (0x45504) bit 3.
3. Wa_14010685332 (cnp/icp/jsp/mcc/tgp/adp PCH): `SOUTH_CHICKEN1` (0xC2000)
   bit 7 `SBCLK_RUN_REFCLK_DIS`.

Resume undoes them in reverse. Apple's framebuffer was written for Macs that
use S3, where the platform powers the display off. Under Deep Idle on this
machine it might leave the display core up. 0.4.3 on macOS (one Deep Idle
sleep, acceleration on):

| | awake | at the sleep / wake action |
|---|---|---|
| `DC_STATE_EN` | 0 (DC5/DC6 never allowed) | **0x8: DC9** |
| `PWR_WELL_CTL1` / `CTL2` | 0x3 / 0x3 | **0 / 0** |
| DC3→DC5, DC5→DC6 | 0, 0 | 0, 0 |
| `SOUTH_CHICKEN1` | 0x901 | **0x901: bit 7 clear** |
| DMC | loaded, but program 0x0b004040, ssp 0x6fc0, htp 0x00a40088 (Linux 2.12: 0x0c004040, 0x86fc0, 0x01a40188) | |

Apple's framebuffer does steps 1 and 2 (display core down, DC9); the one
i915 suspend step missing is the PCH reference clock workaround (step 3).
0.4.4 `-s0ixrefclk` sets it at the Deep Idle sleep action, only in that state,
and clears it at the wake action.

**0.4.4 `-s0ixrefclk` result:** the bit is set for the whole sleep (0x901 →
0x981) and the PMC display block reads 9/FF/FF/80 at the halt, as on Linux,
**but SLP_S0 stays 0**. Every i915-dependent PMC bit now matches Linux at the
halt (0x1CAC block, 0x1424 bit 7, 0x10B4 bit 0, 0x1D80 bits 9/12).

**Correction: no i915 is not the Mac's failure.** Linux with
`module_blacklist=i915` never gets past package **C2** (`package_cstate_show`:
C10 0 before and after a suspend): without its driver the GT never idles.
The Mac reaches PC10 for 99 % of the halt. The display work stays useful (it
proves macOS's display shutdown matches i915's) but does not explain the
missing SLP_S0.

**LPM requirements.** The firmware's requirement table (`PEPD._DSM`
57a6512e fn 1 returns the 0xC0 bytes at GNVS `LPMR`, here 0x41EA3000; Linux's
`substate_requirements` was empty) gives per mode 6 masks. Linux always
enters **S0i3.0 (mode 3)**: `f7fe027e e87c01ef 0000112d 00000000 383fe8c5`.
`ISH_REQ` is required and 1 on Linux in S0i3.0, so 1 = met. `SBR_PG` is not
required. Against the ISH-blacklisted Linux latch (S0ix reached), every
required bit the Mac differs on is Mac 1 / Linux 0: **the Mac meets all
S0i3.0 requirements at C10 entry.**

**Thunderbolt 4 (TCSS).** Linux reaches S0ix with `thunderbolt` blacklisted,
with D3cold forbidden on 00:07.0/07.1/0d.0/0d.2, and with the NHI in D0.
With the root ports and the TB4 xHCI unbound (D0 through suspend) SLP_S0
stays 0, but the package stops at **PC9** (PC10 unchanged): not the Mac's
state, so TB4 is not the Mac's blocker.

**Remaining differences** at the halt (not named by Linux or ACPI): 0x1A00
(0x6178D → 0x160D during the Mac's sleep, constant on Linux), 0x1D80 bit 3,
0x1424, 0x1434, 0x1BC4. With `s0ixfn=0x18` (no LPS0 entry/exit) they change
the same way: **macOS's own sleep path changes them, not S0ixStandby's
`_DSM` calls.** Linux's values during s2idle cannot be read, so they may be
normal sleep-time values.

**Perspective:** Linux S0ix 0.76 W; macOS Deep Idle without SLP_S0
≈ 0.5–1.0 W. The missing SLP_S0 is now worth at most a few tenths of a watt.

## Still to capture

- A 30–60 min run of `s2idle-test.sh` for a tighter power number.
- `s0ix-selftest-tool.sh -s` and `-r off` from the laptop's own X session.
