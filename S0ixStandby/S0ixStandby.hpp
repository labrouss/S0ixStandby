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
	enum { kCStateCount = 9 };
	uint32_t cstateMask {0};                 // which kCStates[] are readable here
	uint64_t csAtSleep[kCStateCount] {}, csAtWake[kCStateCount] {};
	uint64_t tscAtSleep {0}, tscAtWake {0};
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
