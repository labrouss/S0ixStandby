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
	uint32_t pmcAtBoot[1024] {}, pmcAtHalt[1024] {};
	bool pmcHaltValid {false};

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
	bool readCounter(uint64_t &value);

	void onSleep();
	void onWake();
	void publish();
	static void publishTimerFired(OSObject *owner, IOTimerEventSource *sender);
};

#endif // S0IXSTANDBY_HPP
