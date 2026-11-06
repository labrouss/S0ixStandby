//
//  S0ixGPU.cpp
//  S0ixStandby — Intel integrated GPU state across Deep Idle.
//
//  Apple's graphics drivers assume the classic S3 model: the GPU loses power
//  during sleep and wakes blank, so the wake path simply restarts the engines.
//  In Deep Idle (S0ix) the iGPU may keep its power and its state, and a fresh
//  start on top of leftover execlist/context-status state can desynchronise
//  the driver. Linux handles the same situation by resetting the GT engines
//  on resume (i915 intel_gt_sanitize / __intel_gt_reset).
//
//    default        a few GT registers are read in the sleep action and again
//                   in the wake action (before any driver wakes). If the GT
//                   kept its state across a Deep Idle sleep, a full GT reset
//                   (GDRST) follows, before Apple's driver restarts the
//                   engines. The display is a separate reset domain and is
//                   not touched. When no accelerator programmed the engines
//                   (e.g. WhateverXe -wex-noaccel) nothing is touched at wake.
//    -s0ixnogpureset  no reset (registers are still reported).
//    -s0ixgpuprobe  debugging: an idle GPU is probed too (a canary written to
//                   a render-engine general purpose register), and the last
//                   result is kept in NVRAM (nvram s0ix-gpu-last).
//
//  Intel Gen11 (Ice Lake) and Gen12 (Tiger Lake, Rocket Lake, Alder Lake,
//  Raptor Lake) iGPUs only, selected by CPU model. Register offsets and the
//  reset/forcewake sequences follow the Intel graphics PRMs and Linux i915
//  (intel_uncore.c, intel_reset.c).
//
//  SPDX-License-Identifier: BSD-3-Clause
//

#include "S0ixStandby.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IONVRAM.h>
#include <pexpert/pexpert.h>

#define LOG(fmt, ...)  IOLog("S0ixStandby: " fmt "\n", ##__VA_ARGS__)

// --- Registers (MMIO BAR0 offsets) -------------------------------------------

static const uint32_t kRCS = 0x02000, kBCS = 0x22000;    // engine MMIO bases
static const uint32_t kFwRenderReq = 0x0A278, kFwRenderAck = 0x00D84;   // FORCEWAKE_RENDER_GEN9
static const uint32_t kFwGtReq     = 0x0A188, kFwGtAck     = 0x130044;  // FORCEWAKE_GT_GEN9
static const uint32_t kGDRST = 0x0941C;                  // GEN6_GDRST
static const uint32_t kGrdomFull = 1u << 0;              // GEN11_GRDOM_FULL
static const uint32_t kResetCtl = 0xD0;                  // RING_RESET_CTL (engine relative)
static const uint32_t kMaskedEnable0 = 0x00010001, kMaskedDisable0 = 0x00010000;
static const uint32_t kCanary = 0x53304958;              // "S0IX"
static const uint32_t kPmcsrOff = 4;                     // PMCSR at PM capability + 4

struct GpuReg { const char *name; uint32_t off; };
static const GpuReg kGpuRegs[S0ixStandby::kGpuRegCount] = {
	{ "HWS_PGA",   kRCS + 0x080 },   // hardware status page address
	{ "GFX_MODE",  kRCS + 0x29C },   // bit 3 = execlists enabled (driver-programmed)
	{ "EL_STAT_LO", kRCS + 0x234 },  // execlist status
	{ "EL_STAT_HI", kRCS + 0x238 },
	{ "CSB_PTR",   kRCS + 0x3A0 },   // context status buffer read/write pointers
	{ "HEAD",      kRCS + 0x034 },
	{ "TAIL",      kRCS + 0x030 },
	{ "MI_MODE",   kRCS + 0x09C },
	{ "BCS_HWS",   kBCS + 0x080 },
	{ "BCS_MODE",  kBCS + 0x29C },
	{ "GPR0",      kRCS + 0x600 },   // CS general purpose register 0 (canary)
	{ "GDRST",     kGDRST },
};
enum { kRegHws = 0, kRegMode = 1, kRegGpr0 = 10 };

// Gen11/Gen12 iGPU CPUs (family 6 model). Meteor Lake and later have a
// different GT/media layout and are left out.
static const uint8_t kGpuModels[] = {
	0x7D, 0x7E,         // Ice Lake
	0x8C, 0x8D,         // Tiger Lake
	0xA7,               // Rocket Lake
	0x97, 0x9A, 0xBE,   // Alder Lake
	0xB7, 0xBA, 0xBF,   // Raptor Lake
};

static const char *const kVerdictName[] = {
	"unknown", "retained", "lost power", "lost power (PCI config space reset)",
	"not checked (no accelerator active at sleep)",
};
enum { kVerdictCount = 5, kResetCount = 9 };
static const char *const kResetName[] = {
	"not attempted", "done", "skipped: GPU state was lost (nothing to do)",
	"skipped: no verdict", "failed: forcewake timeout", "failed: GDRST timeout",
	"skipped: GPU not accessible at wake", "skipped: no accelerator active (nothing to do)",
	"skipped: not a Deep Idle wake",
};
static const uint32_t kSleepTypeDeepIdleGpu = 7;   // kIOPMSleepTypeDeepIdle

// --- MMIO helpers -------------------------------------------------------------

uint32_t S0ixStandby::gpuRead(uint32_t off)
{
	return *reinterpret_cast<volatile uint32_t *>(gpuMMIO + off);
}

void S0ixStandby::gpuWrite(uint32_t off, uint32_t v)
{
	*reinterpret_cast<volatile uint32_t *>(gpuMMIO + off) = v;
	(void)gpuRead(off);   // posting read
}

bool S0ixStandby::gpuPoll(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeoutUs)
{
	for (uint32_t t = 0; t <= timeoutUs; t += 10) {
		uint32_t v = gpuRead(off);
		if (v == 0xFFFFFFFF) return false;        // device not responding
		if ((v & mask) == want) return true;
		IODelay(10);
	}
	return false;
}

// Forcewake (i915 fw_domain_get/put, FORCEWAKE_KERNEL bit). If the bit is
// already acknowledged, another requester holds the domain: use it and do not
// release it afterwards.
bool S0ixStandby::gpuForcewake(bool on, bool *took)
{
	static const uint32_t req[2] = { kFwRenderReq, kFwGtReq };
	static const uint32_t ack[2] = { kFwRenderAck, kFwGtAck };
	for (int i = 0; i < 2; i++) {
		if (on) {
			took[i] = false;
			uint32_t a = gpuRead(ack[i]);
			if (a == 0xFFFFFFFF) return false;
			if (a & 1) continue;
			gpuWrite(req[i], kMaskedEnable0);
			took[i] = true;
			if (!gpuPoll(ack[i], 1, 1, 50000)) return false;
		} else if (took[i]) {
			gpuWrite(req[i], kMaskedDisable0);
			took[i] = false;
		}
	}
	return true;
}

// The GPU is only accessed in D0. At the wake action macOS's PCI layer may not
// have re-enabled memory decode yet: if BAR0 still holds its value from the
// sleep action (config space intact, no reset), decode is enabled for the
// access and the original command register is restored afterwards. A changed
// BAR0 means the function was reset: never touched.
bool S0ixStandby::gpuAccessible(GpuState &s, const GpuState *ref)
{
	s.cmd = gpuDev->configRead16(kIOPCIConfigCommand);
	s.pmcsr = gpuPmCap ? gpuDev->configRead16(gpuPmCap + kPmcsrOff) : 0;
	s.bar0 = gpuDev->configRead32(kIOPCIConfigBaseAddress0);
	s.bar1 = gpuDev->configRead32(kIOPCIConfigBaseAddress1);
	if ((s.pmcsr & 3) != 0) { s.why = 1; return false; }
	if (s.cmd & kIOPCICommandMemorySpace) return true;
	if (!ref || !ref->valid || (s.bar0 & ~0xFu) == 0 || s.bar0 != ref->bar0 || s.bar1 != ref->bar1) {
		s.why = 3;
		return false;
	}
	gpuDev->configWrite16(kIOPCIConfigCommand, (UInt16)(s.cmd | kIOPCICommandMemorySpace));
	s.decodeEnabled = true;
	return true;
}

void S0ixStandby::gpuRestoreDecode(const GpuState &s)
{
	if (s.decodeEnabled)
		gpuDev->configWrite16(kIOPCIConfigCommand, s.cmd);
}

// --- Setup --------------------------------------------------------------------

void S0ixStandby::gpuTimerFired(OSObject *owner, IOTimerEventSource *)
{
	S0ixStandby *self = OSDynamicCast(S0ixStandby, owner);
	if (!self) return;
	if (self->gpuMMIO) {                      // 60 s after setup: display state while awake
		self->publishDisplay();
		return;
	}
	if (self->setupGPU()) self->gpuTimer->setTimeoutMS(60000);
	else if (self->gpuTries < 6) self->gpuTimer->setTimeoutMS(10000);
}

bool S0ixStandby::setupGPU()
{
	gpuTries++;
	bool supported = false;
	for (uint8_t m : kGpuModels) if (m == cpuModel) supported = true;
	if (!supported) {
		setProperty("GPU", "not supported on this CPU (Gen11/Gen12 Intel iGPU only)");
		gpuTries = 100;   // do not retry
		return false;
	}

	OSDictionary *match = serviceMatching("IOPCIDevice");
	if (!match) return false;
	OSIterator *it = getMatchingServices(match);
	match->release();
	if (!it) return false;
	IOPCIDevice *found = nullptr;
	while (OSObject *o = it->getNextObject()) {
		IOPCIDevice *dev = OSDynamicCast(IOPCIDevice, o);
		if (!dev || dev->getBusNumber() != 0 || dev->getDeviceNumber() != 2 || dev->getFunctionNumber() != 0)
			continue;
		uint32_t id = dev->configRead32(kIOPCIConfigVendorID);
		uint32_t cls = dev->configRead32(kIOPCIConfigClassCode) >> 8;
		if ((id & 0xFFFF) == 0x8086 && ((cls >> 16) & 0xFF) == 0x03) { found = dev; found->retain(); }
		break;
	}
	it->release();
	if (!found) { setProperty("GPU", "Intel iGPU (00:02.0) not found yet"); return false; }

	IOMemoryMap *map = found->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
	if (!map || map->getLength() < 0x140000) {
		OSSafeReleaseNULL(map);
		found->release();
		setProperty("GPU", "Intel iGPU found, MMIO BAR not mappable");
		gpuTries = 100;
		return false;
	}
	UInt8 pm = 0;
	found->findPCICapability(kIOPCIPowerManagementCapability, &pm);

	cleanNvram();
	dispTgl = cpuModel != 0x7D && cpuModel != 0x7E;   // Ice Lake keeps the Gen9 counter layout
	gpuDev = found;
	gpuMap = map;
	gpuPmCap = pm;
	gpuMMIO = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());

	char buf[96];
	uint32_t id = found->configRead32(kIOPCIConfigVendorID);
	snprintf(buf, sizeof(buf), "00:02.0 %04x:%04x, %s%s", id & 0xFFFF, id >> 16,
	         gpuReset ? "reset on Deep Idle wake if state kept" : "no reset (-s0ixnogpureset)",
	         gpuProbe ? ", full probe + NVRAM record" : "");
	setProperty("GPU", buf);
	LOG("GPU %s", buf);
	return true;
}

// --- Sleep / wake -------------------------------------------------------------

void S0ixStandby::gpuCapture(GpuState &s, bool writeCanary, const GpuState *ref)
{
	s = GpuState {};
	if (!gpuMMIO) return;
	if (!gpuAccessible(s, ref)) return;
	bool took[2] {};
	if (!gpuForcewake(true, took)) {
		gpuForcewake(false, took);
		gpuRestoreDecode(s);
		s.why = 2;
		return;
	}
	for (uint32_t i = 0; i < kGpuRegCount; i++)
		s.reg[i] = gpuRead(kGpuRegs[i].off);
	// No accelerator programs the engines (no status page, execlists off):
	// leave a canary in GPR0, nothing else uses it.
	bool active = s.reg[kRegHws] != 0 || (s.reg[kRegMode] & 0x8);
	if (writeCanary && !active) {
		gpuWrite(kGpuRegs[kRegGpr0].off, kCanary);
		s.canary = gpuRead(kGpuRegs[kRegGpr0].off) == kCanary;
	}
	gpuForcewake(false, took);
	s.valid = true;
	// The wake action restores decode after the optional reset (gpuWake).
	if (!ref) gpuRestoreDecode(s);
}

// Sleep action: all drivers are asleep (Apple's accelerator stopped the engines).
void S0ixStandby::gpuSleep()
{
	gpuCapture(gpuAtSleep, gpuProbe, nullptr);
	// Without an accelerator nothing depends on the GT's state: unless the
	// full probe is on, leave the GPU alone at wake.
	gpuIdleAtSleep = gpuAtSleep.valid && !gpuAtSleep.canary &&
	                 gpuAtSleep.reg[kRegHws] == 0 && !(gpuAtSleep.reg[kRegMode] & 0x8);
}

// Wake action: before any driver wakes.
void S0ixStandby::gpuWake()
{
	gpuVerdict = 0;
	gpuResetResult = 0;
	gpuResetUs = 0;
	gpuEnginesReady = 0;
	if (gpuIdleAtSleep && !gpuProbe) {
		gpuAtWake = GpuState {};
		gpuVerdict = 4;                       // accelerator idle: not checked
		if (gpuReset) gpuResetResult = 7;
		return;
	}
	gpuCapture(gpuAtWake, false, &gpuAtSleep);

	if (gpuAtSleep.valid && !gpuAtWake.valid && gpuAtWake.why == 3)
		gpuVerdict = 3;                       // PCI config space was reset
	if (gpuAtSleep.valid && gpuAtWake.valid) {
		if (gpuAtSleep.canary)
			gpuVerdict = gpuAtWake.reg[kRegGpr0] == kCanary ? 1 : 2;
		else if (gpuAtSleep.reg[kRegHws] != 0 || (gpuAtSleep.reg[kRegMode] & 0x8))
			gpuVerdict = ((gpuAtWake.reg[kRegHws] != 0 && gpuAtWake.reg[kRegHws] == gpuAtSleep.reg[kRegHws]) ||
			              (gpuAtWake.reg[kRegMode] & 0x8)) ? 1 : 2;
	}
	if (gpuVerdict == 1) gpuRetainedCount++;

	// Only Deep Idle keeps the GT powered; after S3 it starts blank anyway.
	if (gpuReset) {
		if (lastSleepType == kSleepTypeDeepIdleGpu) gpuResetNow();
		else gpuResetResult = 8;
	}
	gpuRestoreDecode(gpuAtWake);
}

void S0ixStandby::gpuResetNow()
{
	if (!gpuAtWake.valid) { gpuResetResult = 6; return; }
	if (gpuVerdict == 2 || gpuVerdict == 3) { gpuResetResult = 2; return; }
	if (gpuVerdict == 0) { gpuResetResult = 3; return; }

	uint64_t t0 = mach_absolute_time();
	bool took[2] {};
	if (!gpuForcewake(true, took)) { gpuForcewake(false, took); gpuResetResult = 4; return; }
	// Ask the render and blitter engines to stop memory traffic first
	// (i915 gen8_engine_reset_prepare); best effort, they should be idle.
	static const uint32_t engines[2] = { kRCS, kBCS };
	for (uint32_t e = 0; e < 2; e++) {
		gpuWrite(engines[e] + kResetCtl, kMaskedEnable0);
		if (gpuPoll(engines[e] + kResetCtl, 0x2, 0x2, 1000)) gpuEnginesReady |= 1u << e;
	}
	gpuWrite(kGDRST, kGrdomFull);
	bool done = gpuPoll(kGDRST, kGrdomFull, 0, 100000);
	for (uint32_t e = 0; e < 2; e++)
		gpuWrite(engines[e] + kResetCtl, kMaskedDisable0);
	gpuForcewake(false, took);
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	gpuResetUs = (uint32_t)(ns / 1000);
	gpuResetResult = done ? 1 : 5;
	if (done) gpuResetCount++;
}

// --- Report (from the publish timer) -------------------------------------------

static const char *const kNvramVar = "s0ix-gpu-last";

// 0.3.2 used the private force-sync key, which this macOS stores as a plain
// variable instead: remove that leftover once at boot.
void S0ixStandby::cleanNvram()
{
	IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (!opts) return;
	static const char *const junk[] = { "IONVRAM-FORCESYNCNOW-PROPERTY", kIONVRAMSyncNowPropertyKey };
	for (const char *k : junk)
		if (opts->getProperty(k))
			opts->removeProperty(k);
	opts->release();
}

// A graphics driver that fails after wake can panic before anyone can read
// the registry: keep the last result in NVRAM too (nvram s0ix-gpu-last).
// One small write per wake, only with -s0ixgpuprobe.
void S0ixStandby::recordGPU()
{
	char line[200];
	const GpuState &a = snap.gpuAtSleep, &b = snap.gpuAtWake;
	snprintf(line, sizeof(line),
	         "wake %u type %u: %s; reset %u/%uus; sleep v%u HWS=%x MODE=%x GPR0=%x; wake v%u cmd=%x HWS=%x MODE=%x GPR0=%x",
	         snap.sleepCount, snap.sleepType, kVerdictName[snap.gpuVerdict < kVerdictCount ? snap.gpuVerdict : 0],
	         snap.gpuResetResult, snap.gpuResetUs,
	         a.valid, a.reg[kRegHws], a.reg[kRegMode], a.reg[kRegGpr0],
	         b.valid, b.cmd, b.reg[kRegHws], b.reg[kRegMode], b.reg[kRegGpr0]);
	IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (!opts) return;
	bool stored = false, synced = false;
	if (OSData *d = OSData::withBytes(line, (unsigned)strlen(line))) {
		stored = opts->setProperty(kNvramVar, d);
		d->release();
		// Commit now: the next thing to happen may be a panic. macOS 26
		// stores the sync-now property keys as plain variables, so call
		// IODTNVRAM::sync() directly (as Lilu's NVStorage does).
		if (stored)
			if (IODTNVRAM *nv = OSDynamicCast(IODTNVRAM, opts))
				if (nv->safeToSync()) {
					nv->sync();
					synced = true;
				}
	}
	opts->release();
	setProperty("GPU NVRAM record", stored ? (synced ? "stored and committed" : "stored, not committed (not safe to sync)")
	                                       : "refused");
}

void S0ixStandby::gpuFormat(const GpuState &s, char *out, size_t size)
{
	static const char *const why[] = { "", "not in D0", "forcewake timeout",
	                                   "memory decode off and BAR0 changed (reset)" };
	if (!s.valid) {
		snprintf(out, size, "%s (cmd 0x%x pmcsr 0x%x bar0 %x:%x)", s.why < 4 ? why[s.why] : "?",
		         s.cmd, s.pmcsr, s.bar1, s.bar0);
		return;
	}
	size_t n = snprintf(out, size, "D%u cmd=%x pmcsr=%x bar0=%x:%x%s", s.pmcsr & 3, s.cmd, s.pmcsr,
	                    s.bar1, s.bar0, s.decodeEnabled ? " (decode was off, enabled for the read)" : "");
	for (uint32_t i = 0; i < kGpuRegCount && n < size; i++)
		n += snprintf(out + n, size - n, " %s=%x", kGpuRegs[i].name, s.reg[i]);
	if (s.canary && n < size) snprintf(out + n, size - n, " (canary written)");
}

void S0ixStandby::publishGPU()
{
	if (!gpuMMIO) return;
	char buf[400];
	gpuFormat(snap.gpuAtSleep, buf, sizeof(buf));
	setProperty("GPU registers at sleep", buf);
	if (snap.gpuVerdict == 4) snprintf(buf, sizeof(buf), "not read (no accelerator active at sleep)");
	else gpuFormat(snap.gpuAtWake, buf, sizeof(buf));
	setProperty("GPU registers at wake", buf);

	const char *how = snap.gpuAtSleep.canary ? " (canary)" : " (status page / execlist mode)";
	snprintf(buf, sizeof(buf), "%s%s", kVerdictName[snap.gpuVerdict < kVerdictCount ? snap.gpuVerdict : 0],
	         (snap.gpuVerdict == 1 || snap.gpuVerdict == 2) ? how : "");
	setProperty("GPU state after sleep", buf);
	setProperty("GPU retained count", (unsigned long long)gpuRetainedCount, 32);

	if (gpuReset) {
		if (snap.gpuResetResult == 1 || snap.gpuResetResult == 5)
			snprintf(buf, sizeof(buf), "%s in %u us (engines ready 0x%x)",
			         kResetName[snap.gpuResetResult], snap.gpuResetUs, snap.gpuEnginesReady);
		else
			snprintf(buf, sizeof(buf), "%s", kResetName[snap.gpuResetResult < kResetCount ? snap.gpuResetResult : 0]);
		setProperty("GPU reset", buf);
		setProperty("GPU reset count", (unsigned long long)gpuResetCount, 32);
	}
	if (gpuProbe) recordGPU();
	LOG("wake #%u: GPU %s%s%s", snap.sleepCount, kVerdictName[snap.gpuVerdict < kVerdictCount ? snap.gpuVerdict : 0],
	    gpuReset ? ", reset " : "", gpuReset ? kResetName[snap.gpuResetResult < kResetCount ? snap.gpuResetResult : 0] : "");
}

// --- Display power state (read-only) ------------------------------------------
// S0ix needs the display in DC6, which the display microcontroller (DMC)
// firmware enters on behalf of the driver. On Linux, booting without i915
// leaves the PMC's display handshake unset and the SLP_S0 counter at 0.
// These reads show whether macOS's framebuffer loaded the DMC firmware and
// whether the display ever reaches DC5/DC6. Same registers as Linux's
// i915_dmc_info. Nothing is written.

enum { kDispProg, kDispSsp, kDispHtp, kDispDcEn, kDispDc5, kDispDc6, kDispDc3co, kDispPw1, kDispPw2 };

void S0ixStandby::dispCapture(DispState &d, const DispState *ref)
{
	d = DispState {};
	if (!gpuMMIO || !gpuDev) return;
	uint16_t cmd = gpuDev->configRead16(kIOPCIConfigCommand);
	uint16_t pmcsr = gpuPmCap ? gpuDev->configRead16(gpuPmCap + kPmcsrOff) : 0;
	d.bar0 = gpuDev->configRead32(kIOPCIConfigBaseAddress0);
	d.bar1 = gpuDev->configRead32(kIOPCIConfigBaseAddress1);
	if ((pmcsr & 3) != 0) { d.why = 1; return; }
	bool enabled = false;
	if (!(cmd & kIOPCICommandMemorySpace)) {
		// Same rule as the GT probe: only with config space unchanged since sleep.
		if (!ref || !ref->valid || (d.bar0 & ~0xFu) == 0 || d.bar0 != ref->bar0 || d.bar1 != ref->bar1) {
			d.why = 2;
			return;
		}
		gpuDev->configWrite16(kIOPCIConfigCommand, (UInt16)(cmd | kIOPCICommandMemorySpace));
		enabled = true;
	}
	const uint32_t off[kDispRegCount] = {
		0x80000,                                   // DMC_PROGRAM(0): first firmware word
		0x8F074,                                   // DMC_SSP_BASE
		0x8F004,                                   // DMC_HTP_SKL
		0x45504,                                   // DC_STATE_EN
		dispTgl ? 0x101084u : 0x80030u,            // DC3 -> DC5 count
		dispTgl ? 0x101088u : 0x8002Cu,            // DC5 -> DC6 count
		dispTgl ? 0x101090u : 0u,                  // DC3CO count (Gen12)
		0x45400,                                   // HSW_PWR_WELL_CTL1 (BIOS)
		0x45404,                                   // HSW_PWR_WELL_CTL2 (driver)
	};
	for (uint32_t i = 0; i < kDispRegCount; i++)
		d.reg[i] = off[i] ? gpuRead(off[i]) : 0;
	if (enabled) gpuDev->configWrite16(kIOPCIConfigCommand, cmd);
	d.valid = true;
}

void S0ixStandby::dispFormat(const DispState &d, char *out, size_t size)
{
	if (!d.valid) {
		snprintf(out, size, "not read (%s)", d.why == 1 ? "iGPU not in D0" : d.why == 2 ? "memory decode off" : "no iGPU");
		return;
	}
	uint32_t dc = d.reg[kDispDcEn];
	snprintf(out, size, "DC_STATE_EN 0x%08x (%s), DC3->DC5 %u, DC5->DC6 %u, DC3CO %u, PWR_WELL_CTL1 0x%08x, CTL2 0x%08x",
	         dc, (dc & 3) == 2 ? "DC6 allowed" : (dc & 3) == 1 ? "DC5 allowed" : "DC5/DC6 off",
	         d.reg[kDispDc5], d.reg[kDispDc6], d.reg[kDispDc3co], d.reg[kDispPw1], d.reg[kDispPw2]);
}

void S0ixStandby::publishDisplay()
{
	if (!gpuMMIO) return;
	DispState now;
	dispCapture(now, nullptr);
	char buf[256];
	if (now.valid) {
		bool loaded = now.reg[kDispProg] != 0 && now.reg[kDispProg] != 0xFFFFFFFF;
		snprintf(buf, sizeof(buf), "%s: program 0x%08x, ssp base 0x%08x, htp 0x%08x",
		         loaded ? "loaded" : "not loaded", now.reg[kDispProg], now.reg[kDispSsp], now.reg[kDispHtp]);
		setProperty("Display DMC firmware", buf);
	}
	dispFormat(now, buf, sizeof(buf));
	setProperty("Display power now", buf);
	if (snap.sleepCount) {
		dispFormat(snap.dispAtSleep, buf, sizeof(buf));
		setProperty("Display power at sleep", buf);
		dispFormat(snap.dispAtWake, buf, sizeof(buf));
		setProperty("Display power at wake", buf);
		if (snap.dispAtSleep.valid && snap.dispAtWake.valid)
			LOG("wake #%u: display DC5->DC6 +%u during sleep", snap.sleepCount,
			    snap.dispAtWake.reg[kDispDc6] - snap.dispAtSleep.reg[kDispDc6]);
	}
}
