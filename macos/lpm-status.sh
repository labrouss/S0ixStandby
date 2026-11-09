#!/bin/bash
# lpm-status.sh — PMC low-power-mode (LPM) requirement status, macOS vs Linux
# (Tiger Lake-LP PCH).
#
# Needs S0ixStandby >= 0.4.2 with -s0ixpmcdump and one sleep. The kext
# snapshots the PMC page that holds the SLP_S0 counter (PWRM+0x1000) at boot,
# just before the Deep Idle halt and right after it.
#
# Two register sets (Linux drivers/platform/x86/intel/pmc, located on this
# machine with /dev/mem dumps):
#   latched  PWRM+0x1C3C..0x1C50  state captured when the package enters C10
#            (Linux lpm_latch_mode "c10", the default) -> Linux
#            substate_status_registers
#   live     PWRM+0x1C5C..0x1C70  current state -> substate_live_status_registers
# For most requirement bits 1 = requirement met / block off (e.g. CPU_C10_REQ
# is 0 awake and 1 in C10).
#
#   bash macos/lpm-status.sh

P=$(ioreg -rd1 -c S0ixStandby -w0)
MODEL=$(echo "$P" | sed -n 's/.*"CPU model" = \([0-9]*\).*/\1/p')
case "$MODEL" in
  140|141) ;;
  *) echo "This decoder is for Tiger Lake-LP (CPU model 140/141); this CPU reports model ${MODEL:-unknown}."
     echo "Register offsets and bit names differ on other platforms: the comparison would be meaningless."
     exit 1;;
esac
hexof() { echo "$P" | sed -n "s/.*\"$1\" = <\([0-9a-f]*\)>.*/\1/p"; }
BOOT=$(hexof "PMC page at boot"); HALT=$(hexof "PMC page at halt"); AFTER=$(hexof "PMC page after halt")
[ -n "$BOOT" ] || { echo "No PMC dump: boot with -s0ixpmcdump."; exit 1; }
[ -n "$HALT" ] || { echo "No sleep yet: sleep once, then run again."; exit 1; }
[ -n "$AFTER" ] || echo "(no snapshot after the halt: needs S0ixStandby 0.4.2+)"

dw() { local h=${1:$(( $2 * 2 )):8}; [ ${#h} = 8 ] || { echo -1; return; }; echo $(( 16#${h:6:2}${h:4:2}${h:2:2}${h:0:2} )); }
hx() { (( $1 < 0 )) && echo "-" || printf '0x%08x' "$1"; }

# Linux, EliteBook 840 G8 (2026-11-05): latched after s2idle, live while awake
declare -a LIN_LATCH=(0xa5540074 0xac7de3ea 0x1feb 0x1000 0x3996faa5 0xaaaaa)
declare -a LIN_LIVE=(0xa5540074 0xa87c63ea 0x1feb 0x1000 0x39b61aa4 0xaaaaa)

name() {   # register bit -> Linux name
  case "$1:$2" in
    0:18) echo USB2PLL_OFF;; 0:19) echo PCIe/USB3.1_Gen2PLL_OFF;; 0:20) echo PCIe_Gen3PLL_OFF;;
    0:21) echo OPIOPLL_OFF;; 0:22) echo OCPLL_OFF;; 0:23) echo MainPLL_OFF;; 0:24) echo MIPIPLL_OFF;;
    0:25) echo Fast_XTAL_Osc_OFF;; 0:26) echo AC_Ring_Osc_OFF;; 0:27) echo MC_Ring_Osc_OFF;;
    0:29) echo SATAPLL_OFF;; 0:31) echo XTAL_USB2PLL_OFF;;
    1:0) echo CSME_PG;; 1:1) echo SATA_PG;; 1:2) echo xHCI_PG;; 1:3) echo UFSX2_PG;; 1:5) echo OTG_PG;;
    1:6) echo SPA_PG;; 1:7) echo SPB_PG;; 1:8) echo SPC_PG;; 1:9) echo SPD_PG;; 1:10) echo SPE_PG;;
    1:11) echo SPF_PG;; 1:13) echo LSX_PG;; 1:14) echo P2SB_PG;; 1:15) echo PSF_PG;; 1:16) echo SBR_PG;;
    1:17) echo OPIDMI_PG;; 1:18) echo THC0_PG;; 1:19) echo THC1_PG;; 1:20) echo GBETSN_PG;;
    1:21) echo GBE_PG;; 1:22) echo LPSS_PG;; 1:23) echo MMP_UFSX2_PG;; 1:24) echo MMP_UFSX2B_PG;; 1:25) echo FIA_PG;;
    2:0) echo ADSP_D3;; 2:1) echo SATA_D3;; 2:2) echo xHCI0_D3;; 2:5) echo xDCI1_D3;; 2:6) echo SDX_D3;;
    2:7) echo EMMC_D3;; 2:8) echo IS_D3;; 2:9) echo THC0_D3;; 2:10) echo THC1_D3;; 2:11) echo GBE_D3;; 2:12) echo GBE_TSN_D3;;
    3:12) echo DTS_VNN_REQ;;
    4:0) echo CPU_C10_REQ;; 4:3) echo PCIe_LPM_En_REQ;; 4:5) echo ITH_REQ;; 4:6) echo CNVI_REQ;; 4:7) echo ISH_REQ;;
    4:10) echo USB2_SUS_PG_Sys_REQ;; 4:12) echo PCIe_Clk_REQ;; 4:16) echo MPHY_Core_DL_REQ;;
    4:17) echo Break-even_En_REQ;; 4:18) echo Auto-demo_En_REQ;; 4:22) echo MPHY_SUS_REQ;; 4:24) echo xDCI_attached_REQ;;
    *) echo "(unnamed)";;
  esac
}

diffbits() {   # $1 = reg index, $2 = mac value, $3 = Linux value
  (( $2 < 0 )) && return
  local x=$(( $2 ^ $3 )) n
  for n in $(seq 0 31); do
    (( (x >> n) & 1 )) && printf '  %-13s %-4s %-26s Linux %s  mac %s\n' "LPM_STATUS_$1" "$n" "$(name "$1" "$n")" $(( ($3 >> n) & 1 )) $(( ($2 >> n) & 1 ))
  done
}

echo "$P" | grep -E '"(Sleep count|Last sleep type)"' | sed 's/^ *//; s/"//g'
echo
echo "== Latched (state at package C10 entry): the comparison that matters"
printf '%-13s %-12s %-14s %-14s\n' "register" "Linux S0ix" "mac at boot" "mac after halt"
for i in 0 1 2 3 4 5; do
  printf '%-13s %-12s %-14s %-14s\n' "LPM_STATUS_$i" "$(hx $(( LIN_LATCH[i] )))" "$(hx "$(dw "$BOOT" $(( 0xC3C + 4 * i )))")" \
    "$(hx "$(dw "$AFTER" $(( 0xC3C + 4 * i )))")"
done
if [ -n "$AFTER" ]; then
  all0=1; for i in 0 1 2 3 4 5; do (( $(dw "$AFTER" $(( 0xC3C + 4 * i ))) != 0 )) && all0=0; done
  if (( all0 )); then
    echo "  -> still all zero after the halt: the PMC never latched, i.e. it never saw a package C10 entry."
  else
    echo "Bits that differ from Linux:"
    for i in 0 1 2 3 4 5; do diffbits $i "$(dw "$AFTER" $(( 0xC3C + 4 * i )))" $(( LIN_LATCH[i] )); done
  fi
fi
echo
echo "== Live, awake (mac at boot vs Linux awake)"
for i in 0 1 2 3 4 5; do diffbits $i "$(dw "$BOOT" $(( 0xC5C + 4 * i )))" $(( LIN_LIVE[i] )); done
echo
echo "== Live, just before the halt (mac) vs Linux awake: what the sleep already changed"
for i in 0 1 2 3 4 5; do diffbits $i "$(dw "$HALT" $(( 0xC5C + 4 * i )))" $(( LIN_LIVE[i] )); done
