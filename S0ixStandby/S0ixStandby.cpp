//
//  S0ixStandby.cpp
//  S0ixStandby — Modern Standby (S0ix) support for macOS "Deep Idle" sleep.
//
//  How it fits together (see docs/RESEARCH-2A.md):
//    * An SSDT (acpi/SSDT-DEEPIDLE) makes AppleACPIPlatform publish
//      IOPMDeepIdleSupported; macOS then sleeps with IOPMSystemSleepType 7
//      ("Deep Idle", XNU acpi_idle_kernel()) instead of ACPI S3.
//    * Apple never calls the LPS0 _DSM that PC firmware expects for Modern
//      Standby. This kext calls it from IOPlatformSleepAction (after all
//      drivers slept, before the CPUs halt, thread context, interrupts on) and
//      IOPlatformWakeAction, but only for Deep Idle sleeps.
//    * It also reads the SLP_S0 residency counter described by ACPI LPIT, so
//      every sleep reports how long the platform really spent in S0ix.
//
//  _DSM GUIDs and function numbers follow the Intel/Microsoft/AMD LPS0
//  specifications (Linux drivers/acpi/x86/s2idle.c used only as a cross-check).
//
//  Boot-args:  -s0ixoff    do not load
//              -s0ixnodsm  meter only, never call _DSM
//              -s0ixdbg    verbose logging
//
//  SPDX-License-Identifier: BSD-3-Clause
//

#include "S0ixStandby.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/OSByteOrder.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

OSDefineMetaClassAndStructors(S0ixStandby, IOService)

#define LOG(fmt, ...)  IOLog("S0ixStandby: " fmt "\n", ##__VA_ARGS__)
#define DBG(fmt, ...)  do { if (gDebug) IOLog("S0ixStandby: " fmt "\n", ##__VA_ARGS__); } while (0)

static bool gDebug = false;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// GUIDs in the textual form used by ACPI ToUUID().
static const char *const kDsmGuid[] = {
	"c4eb40a0-6cd2-11e2-bcfd-0800200c9a66",   // Intel LPS0
	"11e00d56-ce64-47ce-837b-1f898f9aa461",   // Microsoft Modern Standby
	"e3f32452-febc-43ce-9039-932122d37721",   // AMD LPS0
};
static const char *const kFamilyName[] = { "Intel", "Microsoft", "AMD" };

// Function numbers
enum : uint32_t {
	kFnQuery = 0,
	// Intel and Microsoft
	kFnDisplayOff = 3, kFnDisplayOn = 4, kFnEntry = 5, kFnExit = 6,
	// Microsoft only
	kFnMsSleepEntry = 7, kFnMsSleepExit = 8,
	// AMD
	kFnAmdEntry = 2, kFnAmdExit = 3, kFnAmdDisplayOff = 4, kFnAmdDisplayOn = 5,
};

// Sleep/wake action priority: high = late in the sleep queue and early in the
// (reversed) wake queue, so _DSM entry runs after other sleep actions and _DSM
// exit before other wake actions.
static const uint32_t kActionPriority = 0x8000;

static const uint32_t kSleepTypeDeepIdle = 7;   // kIOPMSleepTypeDeepIdle

// ACPI ToUUID() byte order: first three fields little-endian, rest as written.
static bool guidToBytes(const char *s, uint8_t out[16])
{
	uint8_t raw[16];
	int n = 0;
	for (const char *p = s; *p && n < 32; p++) {
		if (*p == '-') continue;
		int v;
		if (*p >= '0' && *p <= '9') v = *p - '0';
		else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
		else return false;
		if (n & 1) raw[n / 2] |= (uint8_t)v; else raw[n / 2] = (uint8_t)(v << 4);
		n++;
	}
	if (n != 32) return false;
	static const uint8_t order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
	for (int i = 0; i < 16; i++) out[i] = raw[order[i]];
	return true;
}

static bool bootArg(const char *name)
{
	int dummy = 0;
	return PE_parse_boot_argn(name, &dummy, sizeof(dummy));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool S0ixStandby::start(IOService *provider)
{
	if (bootArg("-s0ixoff")) {
		LOG("disabled by -s0ixoff");
		return false;
	}
	gDebug = bootArg("-s0ixdbg");
	dsmEnabled = !bootArg("-s0ixnodsm");

	if (!IOService::start(provider))
		return false;

	lps0 = OSDynamicCast(IOACPIPlatformDevice, provider);
	if (!lps0) {
		LOG("provider is not an ACPI device");
		return false;
	}
	lps0->retain();

	if (!probeDsm() && dsmEnabled)
		LOG("no supported LPS0 _DSM found on %s; meter only", lps0->getName());

	if (!setupMeter())
		LOG("no usable LPIT SLP_S0 counter; residency will not be reported");

	workLoop = IOWorkLoop::workLoop();
	if (workLoop) {
		publishTimer = IOTimerEventSource::timerEventSource(this, &S0ixStandby::publishTimerFired);
		if (publishTimer && workLoop->addEventSource(publishTimer) != kIOReturnSuccess)
			OSSafeReleaseNULL(publishTimer);
	}

	setProperty("DSM enabled", dsmEnabled);
	setProperty("Sleep count", (unsigned long long)0, 32);

	// Register for the sleep/wake platform action queues. XNU installs these
	// after start() succeeds (IOService.cpp, IOInstallServiceSleepPlatformActions).
	setProperty("IOPlatformSleepAction", (unsigned long long)kActionPriority, 32);
	setProperty("IOPlatformWakeAction", (unsigned long long)kActionPriority, 32);

	registerService();
	LOG("started on %s (Intel 0x%x, Microsoft 0x%x, AMD 0x%x, dsm %s)",
	    lps0->getName(), dsmMask[kIntel], dsmMask[kMicrosoft], dsmMask[kAMD],
	    dsmEnabled ? "on" : "off");
	return true;
}

void S0ixStandby::stop(IOService *provider)
{
	if (publishTimer) {
		publishTimer->cancelTimeout();
		if (workLoop) workLoop->removeEventSource(publishTimer);
	}
	IOService::stop(provider);
}

void S0ixStandby::free()
{
	OSSafeReleaseNULL(publishTimer);
	OSSafeReleaseNULL(workLoop);
	counter = nullptr;
	OSSafeReleaseNULL(counterMap);
	OSSafeReleaseNULL(lps0);
	IOService::free();
}

// ---------------------------------------------------------------------------
// LPS0 _DSM
// ---------------------------------------------------------------------------

IOReturn S0ixStandby::callDsm(Family family, uint32_t function, uint64_t *intResult)
{
	uint8_t guid[16];
	if (!lps0 || !guidToBytes(kDsmGuid[family], guid))
		return kIOReturnBadArgument;

	// _DSM(UUID, Revision, Function, Arguments). Revision 0 for all three
	// families [verify against each spec; HP's _DSM ignores Arg1].
	OSData   *uuid = OSData::withBytes(guid, sizeof(guid));
	OSNumber *rev  = OSNumber::withNumber((unsigned long long)0, 32);
	OSNumber *fn   = OSNumber::withNumber((unsigned long long)function, 32);
	OSArray  *args = OSArray::withCapacity(1);
	if (!uuid || !rev || !fn || !args) {
		OSSafeReleaseNULL(uuid); OSSafeReleaseNULL(rev); OSSafeReleaseNULL(fn); OSSafeReleaseNULL(args);
		return kIOReturnNoMemory;
	}

	OSObject *params[4] = { uuid, rev, fn, args };
	OSObject *result = nullptr;
	IOReturn ret = lps0->evaluateObject("_DSM", &result, params, 4);

	if (ret == kIOReturnSuccess && intResult) {
		*intResult = 0;
		if (OSData *d = OSDynamicCast(OSData, result)) {
			uint64_t v = 0;
			unsigned len = d->getLength() < 8 ? d->getLength() : 8;
			const uint8_t *b = static_cast<const uint8_t *>(d->getBytesNoCopy());
			for (unsigned i = 0; b && i < len; i++) v |= (uint64_t)b[i] << (8 * i);
			*intResult = v;
		} else if (OSNumber *n = OSDynamicCast(OSNumber, result)) {
			*intResult = n->unsigned64BitValue();
		}
	}

	OSSafeReleaseNULL(result);
	uuid->release(); rev->release(); fn->release(); args->release();
	DBG("_DSM %s fn %u -> 0x%x", kFamilyName[family], function, ret);
	return ret;
}

bool S0ixStandby::probeDsm()
{
	bool any = false;
	for (uint32_t f = 0; f < kFamilyCount; f++) {
		uint64_t mask = 0;
		if (callDsm(static_cast<Family>(f), kFnQuery, &mask) == kIOReturnSuccess && (mask & 1)) {
			dsmMask[f] = (uint32_t)mask;
			any = true;
		}
		char key[48];
		snprintf(key, sizeof(key), "DSM %s mask", kFamilyName[f]);
		setProperty(key, (unsigned long long)dsmMask[f], 32);
	}
	return any;
}

void S0ixStandby::runIf(Family family, uint32_t function)
{
	if (dsmMask[family] & (1U << function)) {
		IOReturn r = callDsm(family, function);
		if (r != kIOReturnSuccess)
			LOG("_DSM %s fn %u failed: 0x%x", kFamilyName[family], function, r);
	}
}

// Order follows the specs (and Linux s2idle): display off, then entry.
void S0ixStandby::dsmEntry()
{
	if (dsmMask[kAMD]) runIf(kAMD, kFnAmdDisplayOff);
	else               runIf(kIntel, kFnDisplayOff);
	runIf(kMicrosoft, kFnDisplayOff);

	if (dsmMask[kAMD]) runIf(kAMD, kFnAmdEntry);
	else               runIf(kIntel, kFnEntry);
	runIf(kMicrosoft, kFnMsSleepEntry);
	runIf(kMicrosoft, kFnEntry);
}

// Exact reverse: exit, then display on.
void S0ixStandby::dsmExit()
{
	runIf(kMicrosoft, kFnExit);
	runIf(kMicrosoft, kFnMsSleepExit);
	if (dsmMask[kAMD]) runIf(kAMD, kFnAmdExit);
	else               runIf(kIntel, kFnExit);

	runIf(kMicrosoft, kFnDisplayOn);
	if (dsmMask[kAMD]) runIf(kAMD, kFnAmdDisplayOn);
	else               runIf(kIntel, kFnDisplayOn);
}

// ---------------------------------------------------------------------------
// LPIT SLP_S0 residency meter
// ---------------------------------------------------------------------------

bool S0ixStandby::setupMeter()
{
	IOService *pe = getPlatform();
	OSDictionary *tables = pe ? OSDynamicCast(OSDictionary, pe->getProperty("ACPI Tables")) : nullptr;
	OSData *lpit = tables ? OSDynamicCast(OSData, tables->getObject("LPIT")) : nullptr;
	if (!lpit || lpit->getLength() < 36)
		return false;

	const uint8_t *t = static_cast<const uint8_t *>(lpit->getBytesNoCopy());
	uint32_t tableLen = OSReadLittleInt32(t, 4);
	if (tableLen > lpit->getLength()) tableLen = lpit->getLength();

	// LPI descriptors follow the 36-byte header. Native C-state descriptor:
	//   +0 type(4) +4 length(4) +8 uid(2) +12 flags(4) +16 entry trigger GAS(12)
	//   +28 residency(4) +32 latency(4) +36 residency counter GAS(12) +48 frequency(8)
	for (uint32_t off = 36; off + 56 <= tableLen; ) {
		const uint8_t *d = t + off;
		uint32_t type = OSReadLittleInt32(d, 0), len = OSReadLittleInt32(d, 4);
		if (len < 8 || off + len > tableLen) break;
		uint32_t flags = OSReadLittleInt32(d, 12);
		uint8_t space = d[36], bits = d[37];
		uint64_t addr = OSReadLittleInt64(d, 40), freq = OSReadLittleInt64(d, 48);
		bool disabled = flags & 1, noCounter = flags & 2;

		DBG("LPIT desc type %u flags 0x%x space %u bits %u addr 0x%llx freq %llu",
		    type, flags, space, bits, addr, freq);

		// Prefer the last enabled descriptor with a SystemMemory counter: on
		// Intel client platforms that is the SLP_S0 (S0ix) residency counter.
		if (type == 0 && !disabled && !noCounter && space == 0 && addr && (bits == 32 || bits == 64)) {
			IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(addr, bits / 8, kIODirectionIn);
			if (md) {
				IOMemoryMap *map = md->map(kIOMapInhibitCache);
				md->release();
				if (map) {
					OSSafeReleaseNULL(counterMap);
					counterMap = map;
					counter = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
					counterBits = bits;
					counterFreq = freq;
				}
			}
		}
		off += len;
	}

	if (!counter)
		return false;

	setProperty("Residency counter address",
	            (unsigned long long)counterMap->getPhysicalAddress(), 64);
	setProperty("Residency counter frequency", (unsigned long long)counterFreq, 64);
	uint64_t v = 0;
	readCounter(v);
	LOG("LPIT residency counter at 0x%llx, %u-bit, %llu Hz, now %llu",
	    (unsigned long long)counterMap->getPhysicalAddress(), counterBits, counterFreq, v);
	return true;
}

bool S0ixStandby::readCounter(uint64_t &value)
{
	if (!counter) return false;
	if (counterBits == 64) {
		uint32_t lo, hi, hi2;
		do { hi = counter[1]; lo = counter[0]; hi2 = counter[1]; } while (hi != hi2);
		value = ((uint64_t)hi << 32) | lo;
	} else {
		value = counter[0];
	}
	return true;
}

// ---------------------------------------------------------------------------
// Sleep / wake
// ---------------------------------------------------------------------------

IOReturn S0ixStandby::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                           void *param1, void *param2, void *param3, void *param4)
{
	if (functionName) {
		if (functionName->isEqualTo("IOPlatformSleepAction")) { onSleep(); return kIOReturnSuccess; }
		if (functionName->isEqualTo("IOPlatformWakeAction"))  { onWake();  return kIOReturnSuccess; }
	}
	return IOService::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

void S0ixStandby::onSleep()
{
	lastSleepType = 0;
	if (IOPMrootDomain *root = getPMRootDomain())
		if (OSNumber *n = OSDynamicCast(OSNumber, root->getProperty("IOPMSystemSleepType")))
			lastSleepType = n->unsigned32BitValue();

	counterAtSleep = 0;
	readCounter(counterAtSleep);
	timeAtSleep = mach_absolute_time();

	entered = false;
	if (dsmEnabled && lastSleepType == kSleepTypeDeepIdle &&
	    (dsmMask[kIntel] || dsmMask[kMicrosoft] || dsmMask[kAMD])) {
		dsmEntry();
		entered = true;
	}
	DBG("sleep: type %u, dsm %s, counter %llu", lastSleepType, entered ? "entry" : "skipped", counterAtSleep);
}

void S0ixStandby::onWake()
{
	if (entered) {
		dsmExit();
		entered = false;
	}
	counterAtWake = 0;
	readCounter(counterAtWake);
	timeAtWake = mach_absolute_time();
	sleepCount++;
	pendingPublish = true;

	// Do not touch the registry from the platform-action context.
	if (publishTimer)
		publishTimer->setTimeoutMS(500);
}

void S0ixStandby::publishTimerFired(OSObject *owner, IOTimerEventSource *)
{
	if (S0ixStandby *self = OSDynamicCast(S0ixStandby, owner))
		self->publish();
}

void S0ixStandby::publish()
{
	if (!pendingPublish) return;
	pendingPublish = false;

	uint64_t ticks = 0, residencyUs = 0;
	if (counter) {
		uint64_t mask = counterBits == 64 ? ~0ULL : 0xFFFFFFFFULL;
		ticks = (counterAtWake - counterAtSleep) & mask;   // handles one wrap
		if (counterFreq)
			residencyUs = ticks * 1000000ULL / counterFreq;
		totalResidencyUs += residencyUs;
	}

	// Note: XNU rebases mach_absolute_time over Deep Idle (deep_idle_rebase),
	// so this is NOT the wall-clock sleep length; it is reported for reference.
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(timeAtWake - timeAtSleep, &ns);

	setProperty("Sleep count", (unsigned long long)sleepCount, 32);
	setProperty("Last sleep type", (unsigned long long)lastSleepType, 32);
	setProperty("Last DSM entry/exit", lastSleepType == kSleepTypeDeepIdle && dsmEnabled);
	setProperty("Last residency ticks", (unsigned long long)ticks, 64);
	setProperty("Last residency us", (unsigned long long)residencyUs, 64);
	setProperty("Total residency us", (unsigned long long)totalResidencyUs, 64);
	setProperty("Last action span ns", (unsigned long long)ns, 64);

	LOG("wake #%u: sleep type %u, SLP_S0 residency %llu us (%llu ticks)",
	    sleepCount, lastSleepType, residencyUs, ticks);
}
