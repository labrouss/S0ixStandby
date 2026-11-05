# Draft issue for b00t0x/WhateverXe

> Status: draft, to be posted after the long-sleep tests. Contains no
> machine identifiers.

**Title:** Tiger Lake acceleration panics after wake from macOS Deep Idle (S0ix): GT keeps its state, driver restarts it as if powered off

**Setup**

- Tiger Lake-LP, Iris Xe 80 EU (`8086:9a49`), laptop, macOS 26.7.1 (25G241)
- WhateverXe 0.7.0 (also seen with 0.6.1), WhateverXe Graphics payload for
  Tahoe (`AppleIntelTGLGraphics` 16.0.32), Lilu 1.7.2, `-allow3d
  -disablegfxfirmware -igfxdvmt`
- Sleep is macOS **Deep Idle** (`IOPMSystemSleepType` 7, S0ix-style), not S3:
  the laptop has no working S3. Deep Idle is enabled by an SSDT
  ([S0ixStandby](https://github.com/labrouss/S0ixStandby)).

**Symptom**

With acceleration, every wake panics ~1–6 s after the display comes back;
with `-wex-noaccel` sleep/wake is clean.

- 0.6.1: `r113 context-ID-mismatch` (Apple's CSB validator)
- 0.7.0: `r113 ElementSwitch-second-item-null`, from
  `handleSoftwareContextSwitchInterrupt` → `wrapTGLCSBProcess`
  (`submits=66042 csbs=98933`, render engine), black screen until reboot

**Cause**

The accelerator's wake path is the S3 one:
`IntelAccelerator::setPowerState(on)` → `startGraphicsEngine`
(`initModeRegisters`, `initHardwareStatusPageRegisters`, …), and
`IGScheduler5::systemDidWake` → `resetOnWake` per engine. It assumes the GT
lost power. In Deep Idle it does not when acceleration is active. Read in
the platform wake action, before any driver wakes:

| | at sleep (drivers asleep) | at wake (before drivers) |
|---|---|---|
| RCS `HWS_PGA` (0x2080) | `0x40004000` | `0x40004000` |
| RCS `GFX_MODE` (0x229C) | `0x8` | `0x8` |
| BCS `HWS_PGA` | `0x40006000` | `0x40006000` |

(With `-wex-noaccel` the same probe finds the GT blank after wake: a value
written to RCS GPR0 at sleep is gone.)

So the driver restarts execlists on top of leftover execlist/CSB state, and
the first context switches disagree with the software queue.

**Workaround that works**

A full GT reset at the platform wake action, before the accelerator wakes
(i915 `__intel_gt_reset` sequence: forcewake render+GT, `RING_RESET_CTL`
request on RCS/BCS, `GDRST` = `GEN11_GRDOM_FULL`, wait for clear). Done in
S0ixStandby (default since 0.4.0, opt-out `-s0ixnogpureset`), 46–94 us. Afterwards WhateverXe reads
`GFX_MODE` 0 in its wake init and Apple's restart re-enables execlists.
Result: clean accelerated wakes, Metal 2, CSB counters with no errors,
retries or fallbacks: 4/4 sleeps of 1–40 min, video playback after wake.
S0ixStandby 0.4.0 does this by default on Gen11/Gen12 iGPUs.

**Suggestion**

The equivalent could live in WhateverXe: before `startGraphicsEngine` runs
on wake (e.g. in the `setPowerState(on)` path, or when the previous sleep
was `IOPMSystemSleepType` 7), reset the GT the way i915 does on resume
(`intel_gt_sanitize`). Then no external kext is needed. Happy to test.

Logs and the probe implementation:
`S0ixStandby/S0ixGPU.cpp`, `docs/FINDINGS.md` §9 in the S0ixStandby repo.
