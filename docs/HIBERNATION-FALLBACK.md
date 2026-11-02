# Hibernation fallback (parallel track)

Makes macOS sleep save RAM to disk and power off; the next power-on restores
the session. Works with the EFI's existing `_S3` → soft-off patch: macOS
writes the image, the "S3" entry becomes a clean power-off, and OpenCore
resumes the image at boot.

## What already failed (EFI repo `docs/SLEEP.md`)

Tried with HibernationFixup 1.5.4, RTCMemoryFixup (`rtcfx_exclude=80-AB`),
`DisableRtcChecksum` and OpenCore `HibernateMode` **Auto and NVRAM**:

1. **BIOS 005 CMOS resets on restore.** RTCMemoryFixup only stops
   *macOS's* AppleRTC writes. On restore, **boot.efi** writes the
   hibernation key into RTC 0x80–0xAB **before macOS runs**, which hits HP's
   CMOS settings. **This part is NOT fixed yet.** (The normal-shutdown 005
   is fixed.)
2. **Hung restore** on the distorted preview image.

## Next attempt (untested)

| Setting | Value to try | Targets |
|---|---|---|
| `UEFI > ProtocolOverrides > AppleRtcRam` | `true` | OpenCore filters boot.efi's RTC writes… |
| NVRAM `4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rtc-blacklist` | bytes `80`…`AB` | …to these addresses (fixes 1) |
| boot-arg | `-hbfx-disable-patch-pci` | Restore hang (2) |
| `Misc > Boot > HibernateMode` | `NVRAM` | Keep the key out of RTC entirely |
| Kext | HibernationFixup, after Lilu | |
| FileVault | **Off** while testing | |
| macOS | `sudo pmset -a hibernatemode 25 standby 0 autopoweroff 0 powernap 0 proximitywake 0` | |

Every failed restore so far reset the CMOS. Have the BIOS settings noted
(F10) before trying.

## Expected behaviour

- Sleep: about 10–30 s writing RAM (≈ 16 GB) to the NVMe, then power off.
- Wake: press power → normal boot time → session restored.

## Roll back

`HibernateMode` = `None`, and `sudo pmset -a hibernatemode 0`. Sleep goes
back to a clean power-off.

## Relation to S0ixStandby

Complementary: Deep Idle / S0ixStandby gives fast, low-power sleep.
Hibernation protects the session if the battery runs out, and stays useful
as a "deep" stage (e.g. hibernate after several hours in standby) once sleep
works.
