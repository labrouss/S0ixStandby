/*
 * SSDT-DEEPIDLE — S0ixStandby experiment N1 (phase N, see docs/RESEARCH-2A.md)
 *
 * Advertises Apple "Deep Idle" (S0 sleep) to AppleACPIPlatform, which on real
 * Macs looks for \_SB.LPS0 and \_GPE.LXEN. With both returning One, macOS
 * publishes IOPMDeepIdleSupported = true and may choose sleep type 7
 * (kIOPMSleepTypeDeepIdle): XNU's acpi_idle_kernel() then idles the boot CPU
 * in its deepest C-state instead of entering ACPI S3.
 *
 * Status: EXPERIMENTAL. Test from the USB-stick EFI only. Keep the _S3 -> 7
 * patch: if macOS still picks S3, the laptop powers off cleanly as today.
 *
 * Darwin only: other OSes get Zero (nothing outside macOS calls these names).
 * Before enabling, confirm the HP DSDT does not already define \_SB.LPS0 or
 * \_GPE.LXEN (grep the capture-baseline.sh ACPI dump).
 *
 * Written from scratch for this project. Licence: BSD-3-Clause.
 */
DefinitionBlock ("", "SSDT", 2, "S0IX", "DEEPIDLE", 0x00001000)
{
    Scope (\_SB)
    {
        Method (LPS0, 0, NotSerialized)
        {
            If (_OSI ("Darwin"))
            {
                Debug = "S0IX: \\_SB.LPS0 evaluated"
                Return (One)
            }

            Return (Zero)
        }
    }

    Scope (\_GPE)
    {
        Method (LXEN, 0, NotSerialized)
        {
            If (_OSI ("Darwin"))
            {
                Debug = "S0IX: \\_GPE.LXEN evaluated"
                Return (One)
            }

            Return (Zero)
        }
    }
}
