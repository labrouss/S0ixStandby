# Battery "not full" on AC: dark wakes every few minutes

## Symptom

On AC power with the battery at (about) 100 %, macOS dark-wakes the machine
every 4–5 minutes while it sleeps. `pmset -g log` shows wake requests like:

```
[process=PowerUIAgent request=Maintenance deltaSecs=299 info="com.apple.obc"]
[process=powerd request=UserWake info="com.apple.alarm.user-invisible-GaugingMitigationActions,98"]
```

and powerd repeatedly logs `Invalid notChargingReason, skip battery charging
state update`. On battery, or while it's still charging, the dark wakes come
every 25–60 minutes as usual.

On the HP EliteBook 840 G8 these frequent wake/sleep cycles make a Tiger Lake
framebuffer hang (`Failure during sleep: GFX0() : Some drivers failed to handle
setPowerState`, [FINDINGS §12](FINDINGS.md)) much more likely: every AC sleep at
100 % froze within 15–45 minutes. The fix below removes the extra dark wakes.
On that laptop the remaining normal AC dark wakes can still hang the
framebuffer, so it also needs S0ixStandby's `-s0ixacnortc` on AC. With both,
the same sleeps are clean.

## Cause

`ioreg -rn AppleSmartBattery` on AC at 100 %:

| Key | Value |
|---|---|
| `ExternalConnected` | Yes |
| `IsCharging` | No |
| `FullyCharged` | **No** |
| `CurrentCapacity` / `MaxCapacity` | **3620 / 3622** |
| `NotChargingReason` | (missing) |

The EC stops charging a few mAh short of the battery's last full charge
capacity and `_BST` reports "neither charging nor discharging". VirtualSMC's
SMCBatteryManager (`ACPIBattery::calculateBatteryStatus`) sets "fully charged"
only when remaining ≥ last full, so macOS gets a battery that's on AC, not
charging, not full, with no reason. powerd's battery gauging and Optimized
Battery Charging (`com.apple.obc`) keep waking the machine to look at it.

This isn't HP-specific: any EC that ends charging just below "last full" does
the same.

## Detection

S0ixStandby (0.5.2) checks the power source every minute and publishes it.
It only reads, nothing is changed:

```
sudo ./macos/s0ix-status.sh      # section "== Battery"
"Battery full state" = "stuck: on AC, not charging, 3620/3622, FullyCharged No: ..."
```

`stuck` means: on AC, not charging, not FullyCharged, and within 3 % of
`MaxCapacity`. It's also logged once when the state starts or ends.

## Fix: SSDT-BATFULL

[`acpi/SSDT-BATFULL.dsl`](../acpi/SSDT-BATFULL.dsl) wraps the battery's
`_BST`. Under macOS only, when the battery is neither charging nor discharging
and remaining is within 3 % of last full, it returns remaining = last full.
SMCBatteryManager then reports `FullyCharged = Yes`. Everything else passes
through unchanged.

It needs two OpenCore entries:

1. **ACPI → Add**: `SSDT-BATFULL.aml` (compiled by CI, or `iasl SSDT-BATFULL.dsl`).
2. **ACPI → Patch**: rename that battery's `_BST` to `XBST` in the DSDT.

Without the rename, macOS rejects the SSDT (duplicate `_BST`) and nothing
changes, so a wrong patch does no harm.

### Adapting it to another machine

1. **Battery path.** Disassemble your DSDT (`iasl -d DSDT.aml`) and find the
   `PNP0C0A` device that has `_BST`: e.g. `\_SB.BAT0`,
   `\_SB.PCI0.LPCB.EC0.BAT0`, `\_SB.PCI0.LPCB.EC0.BAT1`. Replace `\_SB.BAT0` /
   `_SB_.BAT0` in the four places in the `.dsl`.
2. **Rename patch.** Run
   ```
   python3 tools/find-bst.py DSDT.aml
   ```
   It prints a unique `Find`/`Replace` pair (hex and base64) for each `_BST`
   method. Use the one for your battery (same order as in the disassembly).
3. **No `_BIX`?** If the battery only has `_BIF`, use `_BIF ()` and index
   `One` for the last full charge capacity.
4. Rebuild, reboot, and after about 2 minutes check:
   ```
   ioreg -rn AppleSmartBattery | grep -E "FullyCharged|IsCharging|CurrentCapacity|MaxCapacity"
   ```
   At ~100 % on AC: `FullyCharged = Yes`. The battery menu says
   "Battery Is Charged". In `pmset -g log`, `GaugingMitigationActions`
   disappears from the wake requests and `com.apple.obc` moves out to about
   a day.

### HP EliteBook 840 G8 values

| | |
|---|---|
| Battery | `\_SB.BAT0` (`_BST` = `Return (BTST (Zero))`) |
| Find | `140C5F42535400A44254535400` (`FAxfQlNUAKRCVFNUAA==`) |
| Replace | `140C5842535400A44254535400` (`FAxYQlNUAKRCVFNUAA==`) |
| TableSignature / Count | `DSDT` (`RFNEVA==`) / 1 |

Result on AC at 100 %:

| | Dark wakes | Result |
|---|---|---|
| neither | every ~4 min (gauging + OBC) | every sleep froze within 15–45 min |
| `-s0ixacnortc` only | none while asleep, but a direct sleep after a wake froze twice | froze |
| SSDT-BATFULL only | 5 in 2 h (3× OBC right after the sleep, calendar, CSPN), 45 s each | froze on the re-sleep after the 5th |
| **both** | none while asleep | **clean**: 4/4 short sleeps (up to 98 min) and an **11 h 20 min** night |

The SSDT is useful on any machine that shows `stuck`, because it removes the
frequent dark wakes. Whether `-s0ixacnortc` is also needed depends on whether
the graphics driver survives dark-wake re-sleeps on AC ([TESTING](TESTING.md)).

## Upstream

The proper fix is in SMCBatteryManager: treat "not charging on AC within a
small tolerance of last full" as fully charged. Once that's fixed upstream,
SSDT-BATFULL is no longer needed.
