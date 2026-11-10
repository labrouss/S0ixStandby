/*
 * SSDT-BATFULL: report "fully charged" when the EC stops charging just short
 * of the battery's last full charge capacity. See docs/BATTERY-FULL.md.
 *
 * Problem: many ECs end charging a few mAh below "last full" (HP EliteBook
 * 840 G8: 3620 of 3622) and _BST then reports "neither charging nor
 * discharging". VirtualSMC's SMCBatteryManager sets FullyCharged only when
 * remaining >= last full, so macOS sees a battery on AC that is not
 * charging, not full and has no reason. powerd ("Invalid notChargingReason",
 * GaugingMitigationActions) and PowerUIAgent (com.apple.obc) then dark-wake
 * the machine every few minutes.
 *
 * Fix: under macOS only, when _BST says "not charging and not discharging"
 * and remaining is within 3 % of _BIX's last full charge capacity, return
 * remaining = last full. Everything else passes through unchanged.
 *
 * Adapt per machine (docs/BATTERY-FULL.md, tools/find-bst.py):
 *   1. the battery path below (\_SB.BAT0 here; others use e.g.
 *      \_SB.PCI0.LPCB.EC0.BAT0 or BAT1), in all four places;
 *   2. the OpenCore ACPI patch that renames that battery's _BST to XBST.
 *      HP EliteBook 840 G8: Find 140C5F42535400A44254535400
 *                           Replace 140C5842535400A44254535400 (Count 1, DSDT)
 *   3. if the battery only has _BIF (no _BIX), use _BIF and index One
 *      (last full charge capacity is _BIF[1], _BIX[3]).
 * Without the rename, macOS rejects this SSDT (duplicate _BST) and nothing
 * changes, so a wrong patch is harmless.
 */
DefinitionBlock ("", "SSDT", 2, "S0IX", "BATFULL", 0x00000000)
{
    External (_SB_.BAT0, DeviceObj)
    External (_SB_.BAT0.XBST, MethodObj)
    External (_SB_.BAT0._BIX, MethodObj)

    Scope (\_SB.BAT0)
    {
        Name (FBST, Package (0x04) { Zero, Zero, Zero, Zero })

        Method (_BST, 0, Serialized)  // _BST: Battery Status
        {
            Local0 = XBST ()
            If (!_OSI ("Darwin"))
            {
                Return (Local0)
            }

            Local1 = DerefOf (Local0 [Zero])        // state
            If ((Local1 & 0x03))                    // charging or discharging
            {
                Return (Local0)
            }

            Local2 = DerefOf (Local0 [0x02])        // remaining capacity
            Local3 = _BIX ()
            Local4 = DerefOf (Local3 [0x03])        // last full charge capacity
            If (((Local2 == Zero) || (Local2 >= 0x7FFFFFFF) ||
                 (Local4 == Zero) || (Local4 >= 0x7FFFFFFF) ||
                 (Local2 >= Local4)))
            {
                Return (Local0)
            }

            // Within 3 % of full and not charging on its own: report full.
            If (((Local2 * 0x64) >= (Local4 * 0x61)))
            {
                FBST [Zero] = Local1
                FBST [One] = DerefOf (Local0 [One])
                FBST [0x02] = Local4
                FBST [0x03] = DerefOf (Local0 [0x03])
                Return (FBST)
            }

            Return (Local0)
        }
    }
}
