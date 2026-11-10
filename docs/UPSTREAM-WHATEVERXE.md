# Draft issue for b00t0x/WhateverXe

> Status: draft 1 of 3 (acceleration after wake, fixed on our side by the GPU reset in S0ixStandby 0.4.0). Contains no
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
retries or fallbacks: 4/4 sleeps of 1–40 min, video playback after wake,
then a 4 h sleep with 5 background (dark) wakes, 6/6 resets, no panic.
S0ixStandby 0.4.0 does this by default on Gen11/Gen12 iGPUs.

**Suggestion**

The equivalent could live in WhateverXe: before `startGraphicsEngine` runs
on wake (e.g. in the `setPowerState(on)` path, or when the previous sleep
was `IOPMSystemSleepType` 7), reset the GT the way i915 does on resume
(`intel_gt_sanitize`). Then no external kext is needed. Happy to test.

Logs and the probe implementation:
`S0ixStandby/S0ixGPU.cpp`, `docs/FINDINGS.md` §9 in the S0ixStandby repo.

---

# Draft issue 2: GPU hang/restart loop on hardware video decode (TGL)

> Status: ready to post (2026-11-09). Attach one `.gpuRestart` report.

**Title:** Tiger Lake 9a49: VideoToolbox decode hangs on an unstarted CCS ring, GPU restart every ~10 s; request a boot-arg to disable video acceleration

**Setup**

- HP EliteBook 840 G8, i7-1185G7 (Tiger Lake-U), Iris Xe `8086:9a49`
- macOS 26.7.1 (25G241), OpenCore 1.0.8, Lilu 1.7.2
- WhateverXe 0.8.0, AppleIntelTGLCompat 0.8.0, WhateverXe Graphics payloads
  (`AppleIntelTGLGraphics` / `AppleIntelTGLGraphicsFramebuffer` 16.0.0)
- boot-args include `-allow3d -disablegfxfirmware -igfxdvmt`

**Symptom**

As soon as hardware video decoding is used (YouTube in Safari; Photo Booth
fails similarly), the accelerator keeps detecting GPU hangs and restarting:

```
IOAccelEventMachineFast2::checkGPUProgress() - Signaling hardware error on channel 24..
IOAccelEventMachine2::signalHardwareError(eRestartRequest, int32_t): GPURestartSignaled stampIdx=24 type=2
... same for channel 29 ...
```

| Situation | `GPURestartSignaled` |
|---|---|
| Safari playing YouTube | ~70–76 per 10 min |
| Safari quit (Aerial wallpaper still running) | 0 |
| `AppleIntelTGLGraphicsVADriver.bundle` moved out, Safari playing YouTube (software decode) | 0 |

Each event writes a `Kernel_*.gpuRestart` report (one attached).

**From the `.gpuRestart` report**

- `Application: VTDecoderXPCService` (VideoToolbox)
- "Processing hang for hardware context ID[32]"; hung ring: **CCS**
- Ring status: RCS / CCS / VCS2 / VECS0 "NOT enabled – idle", all their ring
  registers 0; BCS and VCS0 idle (head == tail)
- Stamps: index 24 submitted 5 / returned 4 (BUSY), index 29 submitted 1 /
  returned 0 (BUSY)

So decode-related work seems to land on a ring/engine that is not running on
this GPU and never completes.

**Request**

1. A boot-arg that disables hardware video acceleration (VideoToolbox falls
   back to software) while keeping Metal, as a stable default until the
   decode path works. Today the only way is to remove the VA bundle.
2. If possible, a look at why decode work is submitted to the CCS ring on TGL.

Happy to test builds and provide more reports or tailspins.

---

# Draft issue 3: system freeze when re-entering sleep after a dark wake (TGL, on AC)

> Status: ready to post (updated 2026-11-10).

**Title:** Tiger Lake: hard freeze in `GFX0` `setPowerState` on sleep entry (Deep Idle, mostly on AC), also with `-wex-noaccel`

**Setup**

Same machine and versions as above. Sleep is macOS Deep Idle
(`IOPMSystemSleepType` 7) enabled by an SSDT (the laptop has no working S3);
[S0ixStandby](https://github.com/labrouss/S0ixStandby) adds the Modern
Standby `_DSM` calls. `pmset`: `hibernatemode 0, standby 0, powernap 0,
tcpkeepalive 0, womp 0`.

**Symptom**

On AC power at 100 % charge, macOS's battery management wakes the machine
every ~5 minutes (`PowerUIAgent … info="com.apple.obc"` and
`powerd … GaugingMitigationActions`). Each is a dark wake from Deep Idle; the
framebuffer goes `SetState 1 → 0` (0.5–1.8 s each). After 3–9 such cycles the
next "Entering Sleep state due to 'Maintenance Sleep'" never completes: power
LED steady, no ping, no Caps Lock, no panic. After the forced restart:

```
Failure during sleep: GFX0() : Some drivers failed to handle setPowerState
```

**What it does not depend on** (each tested on AC, sleeping until it froze;
7 runs, all froze after 3–9 dark wakes):

| Configuration | Result |
|---|---|
| default (acceleration, VA bundle, S0ixStandby GPU reset, Wi-Fi D3) | freeze |
| `io=0x800 debug=0x100` | freeze, **no panic** (no `setPowerState` timeout fired) |
| VA bundle removed | freeze |
| + `-wex-noaccel` | freeze |
| + S0ixStandby Wi-Fi D3 off | freeze |
| + no S0ixStandby `_DSM` 5/6 (EC sleep flag) | freeze |
| + **S0ixStandby disabled** (`-s0ixoff`, Deep Idle from the SSDT only) | freeze |
| RTC dark wakes blocked while asleep on AC (S0ixStandby `-s0ixacnortc`) | 2 h asleep with no dark wakes and no freeze; the **next** sleep froze |

It is not limited to the re-sleep after a dark wake. The latest freeze was a
user-requested sleep (`Entering Sleep state due to 'Software Sleep'`, AC,
100 %), ~3 minutes after boot. Display sleep and a 15 s dark-wake linger came
first, then the sleep, then the freeze, with a steady LED. Between 2026-11-05
and 2026-11-10, `pmset -g log` lists **15** `Failure during sleep: GFX0() :
Some drivers failed to handle setPowerState` entries. That is every hang in
that period. No other driver is named.

S0ixStandby's sleep code runs as an `IOPlatformSleepAction`, i.e. only after
all drivers' `setPowerState` calls have returned, so it never ran during these
hangs.

On battery the same machine did an 11.5 h Deep Idle night with 26 dark wakes
cleanly (no acceleration), and 4 h with acceleration and 5 dark wakes.

**Battery state and dark-wake length (2026-11-10)**

At 100 % the EC stops charging just short of full. VirtualSMC then reports
"not charging, not full", and macOS dark-wakes every ~4 min (battery gauging,
Optimized Charging). An SSDT that reports the battery as full removes those
wakes. Even then, the re-sleep after an ordinary AC dark wake (CSPN, the 5th in
2 h) hung `GFX0`. Every dark wake on AC lasted **45 s**. On battery all 17
dark wakes of a 9 h night lasted **0 s**, and battery sleep never hangs. Only
blocking RTC dark wakes on AC as well (S0ixStandby `-s0ixacnortc`) made AC
sleeps clean (4/4, up to 98 min). So the trigger is the framebuffer's re-sleep
after a dark wake that ran long enough to involve it.

**Relation to 0.8.0's dark-wake guard**

0.8.0 adds a guard for the repeated `IOGraphicsAccelerator2::system_will_sleep`
preparation loop after a dark wake (`pendingSleepDisplays`,
`WEXDSIDarkWakeParked`). That looks like this exact case: a framebuffer that
never got a wake notification during the dark wake goes through sleep
preparation again. The guard is gated to ADL-P/ADL-N
(`systemWakeRecovery`, display-13 framebuffers), so it is not active on Tiger
Lake (display 12), and `WEXDSIDarkWakeReady` is not published here. Could the
guard be enabled for TGL? I'm happy to test a build or boot-arg that turns it
on.

**Reading**

What remains in every failing run is macOS Deep Idle with the TGL
framebuffer (`GFX0`) cycling through dark wakes on AC. The README notes that
full system sleep/wake is unverified and on the to-do list; this is a
reproducible case for it (one sleep on AC, ~15–45 min). Happy to run debug
builds or collect anything that would show where the framebuffer's re-sleep
stops.
