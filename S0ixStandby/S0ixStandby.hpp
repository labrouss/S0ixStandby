//
//  S0ixStandby.hpp
//  S0ixStandby — Modern Standby (S0ix) support for macOS "Deep Idle" sleep.
//
//  Generic: binds to any ACPI LPS0 device (_HID/_CID PNP0D80), discovers the
//  supported LPS0 _DSM functions at runtime and finds the SLP_S0 residency
//  counter through the ACPI LPIT table. No hardcoded paths, addresses or IDs.
//
//  SPDX-License-Identifier: BSD-3-Clause
//

#ifndef S0IXSTANDBY_HPP
#define S0IXSTANDBY_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/pci/IOPCIDevice.h>

// Logs with IOLog and keeps the line in the "Recent log" ring (S0ixStandby.cpp).
void s0ixLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

class S0ixStandby : public IOService {
	OSDeclareDefaultStructors(S0ixStandby)

public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	void free() override;

	using IOService::callPlatformFunction;

	// XNU calls this for IOPlatformSleepAction / IOPlatformWakeAction
	// (iokit/Kernel/IOPlatformActions.cpp, IOServicePlatformAction()).
	IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
	                              void *param1, void *param2, void *param3, void *param4) override;

	enum { kGpuRegCount = 12 };              // GT registers sampled by the GPU probe
	struct GpuState {
		bool valid, canary;
		uint8_t why;                         // why not valid
		bool decodeEnabled;                  // memory decode was off; enabled for the access
		uint16_t cmd, pmcsr;
		uint32_t bar0, bar1;                 // PCI BAR0 (GTTMMADR) low/high
		uint32_t reg[kGpuRegCount];
	};
	// Display power state (read-only): DMC firmware and DC5/DC6 counters.
	enum { kDispRegCount = 10 };
	struct DispState {
		bool valid;
		uint8_t why;                         // 1 not D0, 2 decode off
		uint32_t bar0, bar1;
		uint32_t reg[kDispRegCount];
	};

private:
	// LPS0 _DSM GUID families (index into mask/guid tables)
	enum Family : uint32_t { kIntel = 0, kMicrosoft = 1, kAMD = 2, kFamilyCount = 3 };

	IOACPIPlatformDevice *lps0 {nullptr};
	uint32_t dsmMask[kFamilyCount] {};      // function-0 support masks (bit n = function n)
	bool dsmEnabled {true};                 // false with -s0ixnodsm (meter only)
	uint32_t fnAllow {0xFFFFFFFF};          // s0ixfn=<mask>: functions allowed to run
	bool entered {false};                   // _DSM entry sequence ran for this sleep

	// SLP_S0 residency counter from LPIT (SystemMemory GAS only for now)
	IOMemoryMap *counterMap {nullptr};
	volatile uint32_t *counter {nullptr};
	uint8_t  counterBits {0};
	uint64_t counterFreq {0};               // Hz; 0 = unknown
	uint64_t counterAddr {0};               // physical address from LPIT

	// Package / core C-state residency MSRs (TSC rate) + TSC, per sleep.
	enum { kCStateCount = 7 };
	uint32_t cstateMask {0};                 // which kCStates[] are readable here
	uint64_t csAtSleep[kCStateCount] {}, csAtWake[kCStateCount] {};
	uint64_t tscAtSleep {0}, tscAtWake {0};
	uint64_t e2AtSleep {0};

	// Boot-CPU snapshot taken in IOPlatformQuiesceAction / ActiveAction, i.e.
	// right around the Deep Idle halt, on the same core (interrupts off: MSR
	// reads only). Core C6/C7 + package C-states + TSC.
	enum { kHaltCount = 2 + kCStateCount };
	uint64_t haltAtQuiesce[kHaltCount] {}, haltAtActive[kHaltCount] {};
	uint64_t tscAtQuiesce {0}, tscAtActive {0};
	bool haltValid {false};
	void readHalt(uint64_t *out);

	// Optional (-s0ixpmcdump): snapshot of the 4 KB PMC page that holds the
	// SLP_S0 counter (IP power-gating status, LTRs, ...), at boot and at the
	// Deep Idle halt. Read-only MMIO.
	volatile uint32_t *counterPage {nullptr};
	bool pmcDump {false};
	uint32_t pmcAtBoot[1024] {}, pmcAtHalt[1024] {}, pmcAtActive[1024] {};
	bool pmcHaltValid {false}, pmcActiveValid {false};   // before / right after the halt

	// -s0ixd3: PCI endpoints with no driver (e.g. an Intel Integrated Sensor
	// Hub on macOS) stay in D0 and can keep the PCH out of S0ix. They are kept
	// in D3hot (nothing uses them).
	enum { kMaxD3 = 16 };
	bool d3Unbound {false};
	IOLock *d3Lock {nullptr};
	IOPCIDevice *d3Dev[kMaxD3] {};
	UInt8 d3Cap[kMaxD3] {};
	uint32_t d3Count {0};
	IOTimerEventSource *scanTimer {nullptr};
	void scanUnbound();

	// SLP_S0 blocker report: PCH IP power-gating status read at the Deep Idle
	// halt; IPs still powered (except always-on infrastructure) are reported
	// by name. Register location and names come from per-platform data
	// (Info.plist "S0ixPlatforms"), keyed by CPU model; no data = no report.
	volatile uint32_t *gatingPtr {nullptr};
	uint32_t gatingBytes {0};
	OSArray *ipNames {nullptr};
	OSArray *ipIgnore {nullptr};             // platform data: IPs known not to block
	uint32_t gatingAtHalt[8] {};
	bool gatingValid {false};
	void setupBlockerReport(uint32_t cpuModel);
	void publishBlockers();
	void publishLiveBlockers();
	OSArray *blockerNames(const uint8_t *gatingBytesPtr, char *line, size_t lineSize);
	IOTimerEventSource *liveTimer {nullptr};
	static void liveTimerFired(OSObject *owner, IOTimerEventSource *sender);
	void d3Enter();

	static void scanTimerFired(OSObject *owner, IOTimerEventSource *sender);
	void readCStates(uint64_t *out);

	// Per-sleep bookkeeping, published after wake from a timer (not from the
	// platform-action context).
	uint32_t sleepCount {0};
	uint32_t lastSleepType {0};
	uint64_t counterAtSleep {0}, counterAtWake {0};
	uint64_t timeAtSleep {0}, timeAtWake {0};   // mach_absolute_time
	uint64_t totalResidencyUs {0};
	bool pendingPublish {false};

	IOWorkLoop *workLoop {nullptr};
	IOTimerEventSource *publishTimer {nullptr};

	bool probeDsm();
	IOReturn callDsm(Family family, uint32_t function, uint64_t *intResult = nullptr);
	void runIf(Family family, uint32_t function);
	void dsmEntry();
	void dsmExit();

	bool setupMeter();

	// HPET standing comparator-match fix: AppleHPET disables HPET (GEN_CONF b0=0),
	// zeroes MAIN_CNT, and leaves T0..T3 comparators at 0. That standing match
	// holds PWRM+0x1C6C bit 19 low and stalls PCH entry to S0i2.0 / SLP_S0.
	IOMemoryMap *hpetMap {nullptr};
	volatile uint32_t *hpetReg {nullptr};
	uint64_t hpetAddr {0};
	bool hpetFixEnabled {false};
	uint32_t hpetFixCount {0};
	uint32_t hpetFixMask {0};
	void setupHpetFix();
	uint32_t hpetClearMatches(uint32_t *outMask, bool write);
	void publishHpet(uint32_t mask);
	bool readCounter(uint64_t &value);

	// Everything publish() needs, copied at wake. A dark wake may go back to
	// sleep within the same second, before the delayed publish runs; the next
	// sleep then overwrites the live fields, but not this snapshot.
	struct Snapshot {
		uint32_t sleepCount, sleepType;
		uint64_t counterAtSleep, counterAtWake, timeAtSleep, timeAtWake;
		uint64_t csAtSleep[kCStateCount], csAtWake[kCStateCount], tscAtSleep, tscAtWake, e2AtSleep;
		bool haltValid;
		uint64_t haltAtQuiesce[kHaltCount], haltAtActive[kHaltCount], tscAtQuiesce, tscAtActive;
		bool gatingValid;
		uint32_t gatingAtHalt[8];
		bool pmcHaltValid, pmcActiveValid;
		GpuState gpuAtSleep, gpuAtWake;
		uint32_t gpuVerdict, gpuResetResult, gpuResetUs, gpuEnginesReady;
		uint16_t wifiPmcsrSleep, wifiPmcsrWake;
		bool hdaValid;
		bool solValid, solRestored;
		uint16_t solPmBefore, solPmHalt, solPmWake;
		uint16_t hdaPmSleep, hdaPmWake;
		uint32_t hdaPgSaved, hdaPgAfter, hdaCgSaved, hdaCgAfter;
		bool ishValid;
		uint16_t ishPmSleep;
		uint32_t ishFwPre, ishFwPost, ishAckCount;
		bool lpmValid;
		uint32_t lpmEn, lpmRes0Sleep, lpmRes0Wake, lpmRes3Sleep, lpmRes3Wake;
		DispState dispAtSleep, dispAtWake;
	};
	Snapshot snap {};
	uint32_t pmcSnap[1024] {}, pmcSnapActive[1024] {};

	// -s0ixgpuprobe / -s0ixgpureset (S0ixGPU.cpp): Intel Gen11/12 iGPU state
	// across Deep Idle, and an optional GT reset at wake before drivers wake.
	bool gpuProbe {false}, gpuReset {false}, gpuResetForced {false};
	uint32_t cpuModel {0};
	IOPCIDevice *gpuDev {nullptr};
	IOMemoryMap *gpuMap {nullptr};
	volatile uint8_t *gpuMMIO {nullptr};
	UInt8 gpuPmCap {0};
	uint32_t gpuTries {0};
	IOTimerEventSource *gpuTimer {nullptr};
	GpuState gpuAtSleep {}, gpuAtWake {};
	bool gpuIdleAtSleep {false};             // no accelerator programmed the GT
	bool gpuSkipForDriver {false};           // WhateverXe keeps the retained GT: no reset
	uint32_t gpuVerdict {0};                 // 0 unknown, 1 retained, 2 lost power, 3 PCI config reset, 4 not checked
	uint32_t gpuResetResult {0}, gpuResetUs {0}, gpuEnginesReady {0};
	uint32_t gpuResetCount {0}, gpuRetainedCount {0};
	bool setupGPU();
	static void gpuTimerFired(OSObject *owner, IOTimerEventSource *sender);
	uint32_t gpuRead(uint32_t off);
	void gpuWrite(uint32_t off, uint32_t v);
	bool gpuPoll(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeoutUs);
	bool gpuForcewake(bool on, bool *took);
	bool gpuAccessible(GpuState &s, const GpuState *ref);
	void gpuRestoreDecode(const GpuState &s);
	void gpuCapture(GpuState &s, bool writeCanary, const GpuState *ref);
	bool gpuDriverKeepsState();
	void gpuSleep();
	void gpuWake();
	void gpuResetNow();
	void gpuFormat(const GpuState &s, char *out, size_t size);
	void publishGPU();
	DispState dispAtSleep {}, dispAtWake {};
	bool dispTgl {false};                    // Gen12 (TGL+) DMC debug counter layout
	void dispCapture(DispState &d, const DispState *ref);
	void dispFormat(const DispState &d, char *out, size_t size);
	void publishDisplay();
	// -s0ixrefclk: i915's Wa_14010685332 (SOUTH_CHICKEN1 bit 7) for Deep Idle
	bool refclkWA {false};
	uint8_t refclkResult {0};                // 0 off, 1 set+cleared, 2 skipped (display up), 3 not accessible, 4 not a Deep Idle sleep
	bool refclkSet {false};
	uint32_t refclkCount {0}, refclkReadback {0};
	void refclkSleep();
	void refclkWake();
	void recordGPU();
	void cleanNvram();

	// -s0ixwifipm: let IOPCIFamily put the Wi-Fi PCI function into D3 for
	// sleep (IOPCIDevice::enablePCIPowerManagement), as Apple's own drivers
	// do. IOPCIFamily then also runs the device's ACPI _PS3 and restores its
	// config space on wake. PMCSR is sampled at the sleep/wake actions.
	bool wifiPM {false};
	IOPCIDevice *wifiDev {nullptr};
	UInt8 wifiPmCap {0};
	uint32_t wifiTries {0};
	IOTimerEventSource *wifiTimer {nullptr};
	uint16_t wifiPmcsrSleep {0xFFFF}, wifiPmcsrWake {0xFFFF};
	static void wifiTimerFired(OSObject *owner, IOTimerEventSource *sender);
	bool setupWifiPM();

	// -s0ixhdapm: Intel cAVS/HDA (00:1f.3) clock/power gating across Deep Idle.
	// AppleHDAController leaves PCI CGCTL (0x48) at 0x00000000 in D3hot, keeping
	// AC_Ring_Osc, OPIDMI_PG, and GPIO_COM5_VNN_REQ awake (~0.4-0.6 W). Restoring
	// Intel PCH CGCTL bits (default 0x003F01FF; override with s0ixhdacg=<mask>)
	// and PGCTL (ADSPPGD=0, LSRMD=1) in PCI config space during Deep Idle clears
	// those gates and restores the saved values before AppleHDAController wakes.
	uint32_t hdaCgMask {0};
	IOPCIDevice *hdaDev {nullptr};
	UInt8 hdaPmCap {0};
	uint16_t hdaDid {0}, hdaPmSleep {0xFFFF}, hdaPmWake {0xFFFF};
	uint32_t hdaPgSaved {0}, hdaPgAfter {0}, hdaCgSaved {0}, hdaCgAfter {0};
	bool hdaTouched {false}, hdaSleepValid {false};
	uint32_t hdaPgValue {0};
	bool hdaPgSet {false};
	void setupHdaPM();
	// -s0ixsold3: Intel AMT Serial-over-LAN function (00:16.3, class 0x0700)
	// to D3hot for the Deep Idle halt and back to D0 at wake. macOS's
	// Apple16X50Serial keeps it in D0, which keeps CSME from power gating and
	// SLP_S0 at 0 on vPro machines (#13).
	bool solPM {false}, solSearched {false};
	IOPCIDevice *solDev {nullptr};
	UInt8 solPmCap {0};
	uint16_t solDid {0}, solPmBefore {0xFFFF}, solPmHalt {0xFFFF}, solPmWake {0xFFFF};
	uint32_t solCfg[16] {};
	bool solTouched {false}, solSleepValid {false}, solRestored {false};
	void setupSolPM();
	// -s0ixlanpm: every PCI Ethernet function released for the Deep Idle
	// halt (D3hot if left in D0, bus mastering off, Intel DRV_LOAD), #14.
	static constexpr uint32_t kMaxLan = 4;
	struct LanDev {
		IOPCIDevice *dev;
		IOMemoryMap *map;
		volatile uint32_t *reg;     // Intel BAR0 (CTRL_EXT), else null
		UInt8 pmCap, bus, slot, fn;
		uint16_t vid, did, pmSleep, pmHalt, cmdSleep;
		uint32_t ctrlExt;
		uint32_t cfg[16];
		bool d3Set, d3Done, bmeCleared, drvCleared, drvRestore, restored;
		const char *note;
	};
	LanDev lan[kMaxLan] {};
	uint32_t lanCount {0};
	bool lanPM {false}, lanSearched {false}, lanSleepValid {false};
	uint64_t lastResUs {0};   // SLP_S0 residency of the last sleep, for the blocker report
	void setupLanPM();
	void lanSleep();
	void lanWake();
	void lanLate();
	void solSleep();
	void solWake();
	void hdaSleep();
	void hdaWake();

	// -s0ixishpm: Intel Integrated Sensor Hub (00:12.0) ISHTP suspend -> D0i3
	// across Deep Idle (and resume on wake) so ISH_PG / ISH_REQ_STS_7 clear
	// for S0i3.0 (#7). Kept in D0 (never D3hot, which leaves ISH SRAM wedged
	// across warm reboots).
	bool ishPM {false}, ishSearched {false}, ishReady {false}, ishSleepValid {false};
	IOPCIDevice *ishDev {nullptr};
	IOMemoryMap *ishMap {nullptr};
	volatile uint32_t *ishReg {nullptr};
	UInt8 ishPmCap {0};
	uint16_t ishDid {0}, ishPmSleep {0xFFFF};
	uint32_t ishResetId {0xFFFFFFFFu}, ishFwPre {0}, ishFwPost {0}, ishAckCount {0};
	void setupIshPM();
	bool ishWaitDrblClear(uint32_t maxMs);
	void ishSendPacket(const uint32_t *words, uint32_t nWords, uint32_t drbl);
	uint32_t ishPollI2h(uint32_t maxMs, bool replyReset, bool stopOnRxCmpl, bool useSleep);
	void ishSleep();
	void ishWake();

	// Read-only PMC LPM substate residency (S0i2.0 / S0i3.0 on TGL/ADL/RPL)
	bool lpmSupported {false}, lpmSleepValid {false};
	uint32_t lpmEn {0}, lpmRes0Sleep {0}, lpmRes0Wake {0}, lpmRes3Sleep {0}, lpmRes3Wake {0};

	// -s0ixacnortc: no RTC alarm wakes from Deep Idle while on AC
	bool acNoRtc {false}, onAC {false}, rtcBlocked {false};
	uint16_t pm1EnPort {0}, pm1EnSaved {0};
	uint8_t cmosBSaved {0};
	uint32_t rtcBlockCount {0};
	IOTimerEventSource *acTimer {nullptr};

	// Wake sources (read-only by default) and -s0ixecwake: keep the EC's
	// GPE armed during Deep Idle so lid open (EC query) can wake.
	bool ecWake {false}, ecArm {false}, ecArmed {false}, wakeInfoValid {false};
	uint16_t pm1StsPort {0}, gpe0StsPort {0}, gpe0EnPort {0};
	uint8_t gpe0Half {0};                  // bytes in each of the status/enable halves
	uint32_t ecGpe {0xFFFFFFFF};           // EC _GPE number, if a GPE0 bit
	uint8_t ecEnAtHalt {0};                // EC GPE enable bit as macOS left it at the halt
	uint16_t pm1StsAtWake {0};
	// PM1_EN around the halt and the FADT power-button model: shows whether
	// the fixed-feature power button (PWRBTN_EN) was armed for the wake.
	uint16_t pm1EnAtHalt {0}, pm1EnAtWake {0};
	uint32_t fadtFlags {0};
	bool fadtFlagsValid {false};
	bool pwrbtnWake {false}, pwrbtnArmed {false};   // -s0ixpwrbtnwake
	uint8_t gpe0StsAtWake[16] {};
	uint8_t gpe0EnAtWake[16] {};          // GPE0_EN right after the halt: what was armed
	uint32_t ecWakeCount {0};
	const char *ecGpeFrom {nullptr};
	uint32_t ecTries {0};
	bool ecGpeWasOn {true};
	// s0ixtrace=0xNN: stage breadcrumbs in a battery-backed CMOS byte, so the
	// last sleep/wake step reached survives a forced power-off.
	uint8_t traceByte {0};
	void traceSetup();
	void trace(uint8_t stage);
	void setupWakeInfo();
	void findEcGpe();
	void publishEcWake();
	void publishWakeInfo();
	bool setupRtcBlock();
	void updateAC();
	static void acTimerFired(OSObject *owner, IOTimerEventSource *sender);

	// Battery "full" state check: AC, not charging, not FullyCharged, but
	// within 3 % of full makes macOS dark-wake every few minutes.
	bool dispLate {false};
	uint32_t dispLoggedFor {0};
	IOTimerEventSource *battTimer {nullptr};
	uint8_t battState {0xFF};
	void battCheck();
	static void battTimerFired(OSObject *owner, IOTimerEventSource *sender);

	void onSleep();
	void onWake();
	void publish();
	void publishLog();
	static void publishTimerFired(OSObject *owner, IOTimerEventSource *sender);
};

#endif // S0IXSTANDBY_HPP
