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

> Update 2026-11-26: solved. The remaining blockers were the stopped HPET's
> standing comparator match and the AMT SOL function left in D0 (§14); with
> both fixed this HP reaches SLP_S0 99.7 %. The PMC diffs listed below as
> remaining are normal sleep-time values.

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

## 12. Freeze on AC: dark-wake re-sleep, independent of S0ixStandby (2026-11-09)

On AC at 100 % charge macOS schedules a maintenance dark wake every ~5 min
(`PowerUIAgent … com.apple.obc`, `powerd … GaugingMitigationActions`); no
`pmset` switch turns these off here (Optimized Battery Charging is not
offered for this battery). After 3–9 dark wakes the next "Entering Sleep …
Maintenance Sleep" never completes: LED steady, no ping, no Caps Lock, no
panic; `pmset` then reports `Failure during sleep: GFX0() : Some drivers
failed to handle setPowerState`.

| Test on AC | Result |
|---|---|
| default (0.4.4/0.5.0, acceleration, VA, GPU reset, `-s0ixwifipm`) | freeze |
| `io=0x800 debug=0x100` (panic on a `setPowerState` timeout) | freeze, no panic |
| `AppleIntelTGLGraphicsVADriver.bundle` removed | freeze |
| + `-wex-noaccel` (no accelerator, so no GPU reset) | freeze |
| + no `-s0ixwifipm` | freeze |
| + `s0ixfn=0x18` (no `_DSM` 5/6, no EC sleep flag) | freeze |
| + `-s0ixoff` (S0ixStandby not running) | freeze |

On battery: 11.5 h night with 26 dark wakes clean (0.1.12, no acceleration),
4 h with acceleration and 5 dark wakes clean. **Conclusion:** macOS Deep Idle
with Apple's pre-release TGL framebuffer fails in dark-wake re-sleeps on AC;
S0ixStandby is not involved. WhateverXe does not hook the TGL framebuffer's
power path (0.7.0 source: only the ADL-N DSI `setPowerState`) and lists full
sleep/wake as unverified. Reported upstream (docs/UPSTREAM-WHATEVERXE.md,
draft 3). Workaround: sleep on battery.

Separately, hardware video decode hangs the GPU every ~10 s on this setup
(`GPURestartSignaled`, `.gpuRestart`: `VTDecoderXPCService`, hung CCS ring
not enabled); removing the VA bundle stops it (draft 2). It explained the GPU
restarts while awake, not the freeze.

### 12.1 Update (2026-11-10): AC dark-wake re-sleeps, multiplied by the battery state at 100 %

All 15 hangs between 2026-11-05 and 2026-11-10 are the same `Failure during
sleep: GFX0() : Some drivers failed to handle setPowerState`; no other driver
is named. S0ixStandby's sleep code runs as an `IOPlatformSleepAction`, i.e.
after every driver's `setPowerState` has returned, so it never ran during
them. The hang is not limited to the re-sleep after a dark wake: it also
happened on a user sleep right after a full (power-button) wake and ~3 min
after boot. What every hang had in common was **AC with the battery at 100 %**.
AC sleeps that started while still charging (87 %, 83 %) were clean for 2 h
and 88 min, and the battery filled up during them.

At 100 % the HP EC stops charging at 3620 of 3622 mAh. SMCBatteryManager sets
"fully charged" only when remaining ≥ last full, so macOS saw `IsCharging No`,
`FullyCharged No`, no `NotChargingReason`. powerd then logged `Invalid
notChargingReason` and scheduled `GaugingMitigationActions` every 3–4 min;
PowerUIAgent's `com.apple.obc` polled every 299 s. Those are the
every-5-minute dark wakes on AC.

[`SSDT-BATFULL`](../acpi/SSDT-BATFULL.dsl) reports remaining = last full when
the battery isn't charging and is within 3 % of full ([BATTERY-FULL](BATTERY-FULL.md)).
With it, `FullyCharged = Yes`, the gauging wakes are gone and the OBC wake
moves to ~23 h. AC sleeps at 100 % went from "every one froze within 15–45 min"
to 4 of 4 clean (20 s, 20 min, 42 min, 98 min, with `-s0ixacnortc`).

Without `-s0ixacnortc` (SSDT-BATFULL only, `FullyCharged = Yes`, no gauging
wakes) it still hung: 5 dark wakes in 2 h (3× OBC in the first 11 min, then
calendar, CSPN), and the re-sleep after the 5th froze GFX0. Every dark wake on
AC lasted **45 s**. On battery all 17 dark wakes of the night lasted **0 s**.
So any dark wake on AC carries the risk: it runs long enough to bring the
framebuffer up (`SetState 1 → 0`), and its re-sleep is where the TGL framebuffer
hangs. The battery-state bug only multiplied those cycles. On the 840 G8 AC
sleeps need both SSDT-BATFULL and `-s0ixacnortc`; with both they were clean.

With both in place the hang is rare but not gone: after four clean AC sleeps in
a row on 2026-11-12/13 (2.7 h, 2.5 h idle sleep, 7.6 h lid, 10 h), an idle sleep
15 min after a normal wake hung GFX0 at sleep entry (2026-11-13 18:35,
`FullyCharged = Yes`, `-s0ixacnortc`, no dark wake before it). So a plain sleep
entry on AC can hang as well; the two measures only remove most of the
framebuffer power cycles. Battery sleeps have not hung.

Refined on 2026-11-21 with SSDT-LIDWAKE: on **battery** a lid/EC wake became a
45 s dark wake (`due to LID/`), and the re-sleep after it hung `GFX0()`. RTC
dark wakes on battery last 0 s and have never hung. So the trigger is not AC as
such but **a long (45 s) dark wake followed by a re-sleep**: on AC every dark
wake is that long, on battery only the lid/EC ones are.

How the battery state reaches GFX0 isn't proven. The working explanation is
that it multiplies the framebuffer's sleep/wake cycles, and the TGL framebuffer
under WhateverXe hangs in one of them now and then. WhateverXe 0.8.0 adds a
guard for the repeated `system_will_sleep` preparation after a dark wake, but
only for ADL-P/ADL-N (display 13), not TGL.

Ruled out on the way: HP's `_PTS(3)` doesn't touch the EC for S3 (`HPTS`
clears `EC0.ACPI` only for S4/S5), and no AC/battery ACPI event calls into
`GFX0` (`_Q06`, `HWAK`, `PNOT`; `GFX0.AINT` only for brightness and
slate/dock).

### 12.2 Lid wake and the long-dark-wake trigger (2026-11-12 to 2026-11-21)

**Why the lid can't wake.** On the HP the lid is an EC event (`_Q0A` →
`Notify(LID)`) on the EC's GPE 0x6E, and `\_SB.LID` has no `_PRW`. Before the
halt macOS leaves GPE 0x6E off (`EC GPE at last halt = disabled by macOS`), so
only the fixed power button wakes. Setting the bit at the quiesce action
(`-s0ixecwake`) didn't help: the lid event was latched but the CPU stayed
halted. A ThinkPad E16 report (#7) explains it: AppleACPIPlatform rewrites
GPE0_EN from the devices' `_PRW` after the quiesce actions. A first
`-s0ixecwake` build also cleared the bit again at wake, which hung the HP while
awake a minute later (fixed in 0.5.4).

**With a `_PRW` it works.** `acpi/SSDT-LIDWAKE` gives `\_SB.LID` a `_PRW` on
0x6E. `Wake GPEs armed during halt` then lists `GPE 0x69, 0x6d, 0x6e (EC),
0x6f`, and opening the lid wakes the HP (`due to LID/HID Activity`, wake source
`GPE 0x47, GPE 0x6e (EC)`, no PWRBTN). The same approach works on the ThinkPad
E16 and an ADL-P ThinkBook (#4).

**But it exposes the framebuffer hang.** Every EC event is now a wake source:

| Test | What happened |
|---|---|
| AC, menu sleep, lid cycles | one lid wake became a dark wake whose promotion failed: `Darkwake Exit Failure` |
| AC, short clamshell sleeps (24 s, 165 s, 94 s) | clean |
| AC 100 %, lid closed ~50 min | EC dark wake after 31 s (`due to LID/`, 45 s), `GFX0()` hang on the re-sleep |
| battery, overnight | RTC dark wakes 0 s, clean; one `LID/` dark wake of 45 s, `GFX0()` hang on its re-sleep |
| AC charging 82→93 %, lid closed, + WhateverXe `-wex-s0-csb-resync` | EC dark wake every ~80 s (45 s each); ~10 re-sleeps clean, then `GFX0()` hang |

**Conclusion.** The `GFX0()` hang follows **a ~45 s dark wake and its
re-sleep**, on AC or battery. 0 s RTC dark wakes on battery never hung. On AC
every dark wake lasts ~45 s, which is why AC looked like the trigger. The hang
rate is roughly one per 3–15 such cycles, with or without
`-wex-s0-csb-resync` (WhateverXe 0.8.3; it coexists with the GT reset but
doesn't fix this on TGL; on the ADL-P ThinkBook it fixed a different wake
deadlock). SSDT-LIDWAKE is therefore not usable on the 840 G8. It also gives a
fast reproducer for WhateverXe: lid closed on AC while charging hangs within
about 20 minutes.

**DMC.** The HP runs Apple's built-in DMC image (`0x0b004040`, SSP `0x6fc0`).
On a TGL-H35 a silent deadlock at sleep went away once WhateverXe loaded
Intel's image (#3, WhateverXe #157). That is a different symptom from
`GFX0()`, and not known to be related.

## 13. Alder/Raptor Lake: PC2/PC3 ceiling from DPOF in `_DSM` fn 5 (2026-11-22, #7)

Reported by brandonedley on a ThinkPad X1 Yoga Gen 7 (ADL-P i7-1270P, 46A6,
display 13 with real DC9), building on keenerdavid18's DPOF lead on a ThinkPad
E16 Gen 3 (RPL-H).

- Before the first sleep the package reaches PC6/PC8 while awake. After the
  first sleep with every `_DSM` function, PC6+ is gone for the rest of the
  boot, `PC3_DMI_NOT_BLOCKED` (pmtelemetry) counts up, and every Deep Idle
  halt stays at PC2/PC3 with PC10 0 %.
- Lenovo's Intel fn 5 does three things: `GUAM(1)`, `EC.ECNT(3)` (ESLP + a
  GPIO), and `\_SB.PC00.DPOF = 1` (Type-C IOM REGBAR+0x40 bit 17, "all
  monitors off"). Split one by one, only the DPOF write causes it. Once written,
  even briefly, the IOM stays in that state until a reboot, so clearing DPOF
  later doesn't help, and any A/B test later in the same boot is misleading.
- Fixes, both from a fresh boot: `s0ixfn=0x198` (keeps Intel fn 3/4 and
  Microsoft fn 7/8, skips fn 5/6), or keep all functions and stop DPOF from
  being written: an OpenCore rename of the `DPOF` field in the TCSS SSDT
  (`TACK, 1; DPOF, 1` → `TACK, 1; XDPO, 1`) plus an SSDT that defines a plain
  `Name (DPOF, Zero)` in `\_SB.PC00`. With either, the X1 Yoga reached
  **PC10 99 %** in Deep Idle (with his `-s0ixtcssd3` TCSS D3cold patch), about
  1.5 W whole-system average overnight, NVMe not involved.
- **Tiger Lake is different:** the HP 840 G8's fn 5 also writes `DPOF = 1`
  (same reference code, ssdt14), and its halt reaches PC10 99 % anyway. So the
  latch looks specific to the ADL/RPL IOM.
- **Not the only ADL/RPL blocker.** On the ThinkPad E16 Gen 3 (RPL-H 240H,
  NootedGreen, two DRAM-less NVMe drives) DPOF was verifiably never written
  (`s0ixfn=0x198`, or all functions with the DPOF rename; IOM register read
  `0x0005d300`, DPOF clear) and the package still stayed at PC3 99 %. Ruled out
  there as well: TCSS D3cold, the refclk workaround, wake-source arming, the
  `_DSM` calls themselves, keeping the display core up, and the NVMe link
  state (idle links gave PC2 73–79 %, a non-idle link PC3 99 %). At the halt
  the PMC shows OPIO / main PLL / fast crystal not off with aggregate LTR "no
  requirement". Linux s2idle on the same machine reaches PC10 93.7 % and SLP_S0.
  Open; the PMC telemetry diff in §13.1 points to PCH-side activity (the CPU
  spoof is ruled out there).
- **DMC label caveat:** the E16 (ADL-P DMC loaded by NootedGreen) reads
  program[0] `0x0b004040` with SSP base `0x86fc0`. program[0] is a header word
  shared by several images, so from 0.5.9 the kext names the image only on
  Tiger Lake and only when both program[0] and the SSP base match.

### 13.1 E16: the blocker is on the PCH side (2026-11-24, #7)

keenerdavid18 compared the PMC telemetry counters (now `macos/pmc-sleep-diff.sh`) (IOReport group
`pmtelemetry_cpu`, subgroup `cstate_counter`, 16-bit counters) before and after
one ~4 min Deep Idle halt on the E16, and brandonedley ran the same diff on the
X1 Yoga Gen 7 across a PC10 99 % halt.

| Counter, during the halt | E16 (PC3 99 %) | X1Y7 (PC10 99 %) |
|---|---|---|
| `PC2R_DEEP_WAKE_OPI_DMI_SQUELCH` | ~33/s (awake ~0/s) | 0 for the whole boot |
| `PC7_PCH_NACK` | ~22/s (awake 0/s) | ~0 (delta = awake time) |
| `PC0_WAKE_PCH_INT` | ~6/s | ~0 |
| `PC2R_DE_BLOCK_FAIL` | ~31/s | ~0 (≈3000/s while scanning out awake) |
| `PC6_PC8_DE_BUFFER_FILL` | ~11/s | ~0 |
| `PC3_DMI_NOT_BLOCKED` (DPOF latch) | 0 | 0 |

- On the E16, something behind the PCH makes DMI/OPI traffic and interrupts
  about 30 times a second during the halt, and the PCH NACKs every deeper
  package entry. On the X1 these counters are silent.
- The display counters follow the wakeup rate (DE_BLOCK_FAIL ~31/s vs DMI
  squelch wakes ~33/s), while every pipe, plane, transcoder, DBUF and power-well
  register reads 0 under DC9. Most likely each PCH wakeup also re-checks the
  display engine and counts a failure: a consequence, not the cause. This fits
  the E16 staying at PC3 with the display core kept up too.
- A steady ~30 Hz suggests polling. Suspects on ADL/RPL hacks: VoodooI2C in
  polling mode (touchpad GPIO interrupt not pinned), CNVi Wi-Fi/Bluetooth kexts,
  a USB device keeping xHCI busy. To check: remove one at a time and rerun the
  diff until `OPI_DMI_SQUELCH` stays at 0, or read Linux `intel_pmc_core`
  (`substate_status_registers`, `ltr_show`) after an s2idle cycle. See
  TESTING.md "Finding what blocks PC10".
- The E16 also tested brandonedley's Ice Lake CPU spoof (model 126) instead of
  Comet Lake: core RAPL power during the halt dropped from ~1.7 W to ~0.7 W, the
  package still stayed at PC3 99 %. The spoof is not the PC3 cause.
- **X1 Yoga Gen 7 recipe:** from a fresh boot without `-s0ixtcssd3`, PC10 99 %
  with SSDT-NODPOF (DPOF rename), display power wells and DBUF off + DC9, and
  NVMe L1.2/LTR. TCSS D3cold is not needed there (IOM `0x00045300`, TD3C=0,
  DPOF=1 at the register, latch avoided by the rename).

## 14. SLP_S0 = 0 at PC10 99 %: the stopped HPET's standing comparator match (2026-11-25, #11)

Found by brandonedley on the ThinkPad X1 Yoga Gen 7 (ADL-P, macOS 26.7.3),
probably the same cause as the HP 840 G8's missing SLP_S0 (§11).

- AppleHPET stops the HPET (`GEN_CONF` bit 0 = 0), zeroes `MAIN_CNT` and
  leaves the T0–T3 comparators at 0 with their interrupts off. Counter and
  comparators then match permanently. Linux leaves its unused comparators
  at `0xFFFFFFFF`, so a stopped HPET there has no match.
- With that standing match the PMC waits indefinitely in the stage before
  S0i2.0 (`PWRM+0x140C` ≈ 100 % of the PC10 time), and `PWRM+0x1C6C` bit 19
  (an S0i2.0 requirement; `ITSS_CLK_SRC_REQ_STS` in Intel's Meteor Lake map)
  stays 0. S0i2.0 (`0x1C80`) and SLP_S0 (`0x193C`) never count.
- Both directions were checked: writing the comparators to 0 on Linux
  reproduced the macOS signature exactly (PC10 8.585 s, `0x140C` 8.583 s,
  SLP_S0 0); on macOS, setting them to `0xFFFFFFFF` gave real SLP_S0 + S0i2.0
  for 99 %+ of 4 consecutive sleeps (battery and AC, 24.5–144.9 s each).
  AppleHPET didn't rewrite the comparators across sleep or wake.
- The §11 "remaining PMC diffs" (`0x10B4`, `0x1424`, `0x1434`, `0x1A00`,
  `0x1D80` bit 3, `0x1BC4`) turned out to be normal sleep-time values: Linux
  reads the same at suspend; they only differed from Linux's awake pages.
- HDA (`HDA_PGD0/3`) and CNVi in `PPFEAR` don't block S0i2.0 (they matter for
  S0i3.0). With audio loaded, Intel HDA `CGCTL` = 0 (as AppleHDAController
  leaves it) still blocks `AC_Ring_Osc_OFF` and costs ~0.4–0.6 W; Linux keeps
  `0x003F01FF` there (#10).
- **Raptor Lake-U confirmed** (Ivanfangsc, ThinkPad X1 Carbon Gen 11, vPro,
  #13): `-s0ixhpetfix` cleared the same 4 comparators, but SLP_S0 stayed 0
  until the AMT Serial-over-LAN function 00:16.3 was put into D3hot (macOS's
  Apple16X50Serial keeps it in D0, so CSME doesn't power gate; Linux puts it
  in D3hot at suspend). With both: SLP_S0 99.4 % of a 72 s halt. 0.5.14 adds
  `-s0ixsold3` for that.
- In S0ixStandby (0.5.13): `-s0ixhpetfix` clears the matching comparators
  (only disabled timers, only while the HPET is stopped; `GEN_CONF` and
  `MAIN_CNT` untouched) at start, before the `_DSM` entry and at wake.
  `-s0ixhdapm` sets HDA `CGCTL`/`PGCTL` for the halt and restores them at wake.
  Both are opt-in until confirmed on more platforms. Without the boot-arg,
  `HPET comparator fix` reports whether the standing match exists.
- **HP EliteBook 840 G8 (Tiger Lake-U, vPro), 2026-11-26:** the same
  standing match (timers 0xf, `CONF 0x2`, `CNT 0`) and the same AMT SOL
  function (00:16.3, 8086:a0e3, D0 under Apple16X50Serial). With the HPET fix
  alone SLP_S0 stayed 0; with the HPET fix and SOL in D3hot for the halt:
  **SLP_S0 99.7 % of a 240 s halt on battery** (PC10 99 %), the first real
  S0ix on this machine under macOS. The serial port came back after the wake.
- From 0.5.15 both are on by default (`-s0ixnohpetfix`, `-s0ixnosold3` turn
  them off); `-s0ixhdapm` stays opt-in.

## 15. S0i3.0 on Alder Lake-P (2026-11-27, #7, #15)

brandonedley, ThinkPad X1 Yoga Gen 7 (ADL-P). With SLP_S0 working (§14), the
package sat in S0i2.0. Comparing the latched LPM status words with Linux
S0i3.0, macOS missed three S0i3.0 requirements:
- **HDA power gating** (`LPM_STATUS_1[29]`): Intel HDA `PGCTL` = 0
  (`s0ixhdapg=0`), not the `0x1b` AppleHDAController leaves.
- **Audio PLL off** (`LPM_STATUS_0[23]`): HDA `CGCTL` bits 22–23. Bisected:
  `0x003F09FF` → S0i3.0 0 s; `0x00FF01FF` → 98.0 %; Linux's `0x00FB09FF` →
  97.7 % with `Fast_XTAL_Osc_OFF` = 1. Now `-s0ixhdapm`'s default.
- **ISH** (`LPM_STATUS_1[27]`, `LPM_STATUS_4[7]`): the sensor hub has no
  macOS driver; PCI D3hot is not enough (and after D0i3 it wedges the ISH
  SRAM until a cold power-off). The ISHTP suspend message that Linux
  `intel-ishtp` sends makes it enter D0i3 itself (`-s0ixishpm`).
- CNVi is not among the differences (§14).

Result: S0i3.0 96.5 % of a 60-min battery sleep (~0.97 W vs ~1.12 W in
S0i2.0) and 97.6 % of a 5 h sleep on AC, audio fine after wake. Not yet
tried on Tiger Lake (HP ISH 8086:a0fc).

## Still to capture

- A 30–60 min run of `s2idle-test.sh` for a tighter power number.
- `s0ix-selftest-tool.sh -s` and `-r off` from the laptop's own X session.
