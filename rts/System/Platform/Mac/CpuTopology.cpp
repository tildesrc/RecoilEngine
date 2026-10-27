/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "System/Platform/CpuTopology.h"

#include <algorithm>
#include <sys/sysctl.h>
#include <sys/types.h>

#include "System/Log/ILog.h"

namespace cpu_topology {

// Reads an integer sysctl by name, returning `fallback` when the key is absent
// (keys such as hw.l3cachesize and the hw.perflevel* tree only exist on some
// hardware).
static int64_t GetSysctlInt(const char* name, int64_t fallback = 0)
{
	int64_t value = 0;
	size_t len = sizeof(value);

	if (sysctlbyname(name, &value, &len, nullptr, 0) != 0)
		return fallback;

	// Some keys report 32 bits even though a 64-bit buffer was offered.
	if (len == sizeof(int32_t))
		value = static_cast<int32_t>(value);

	return value;
}

static int64_t GetPerfLevelInt(uint32_t level, const char* leaf, int64_t fallback = 0)
{
	char name[64];
	snprintf(name, sizeof(name), "hw.perflevel%u.%s", level, leaf);
	return GetSysctlInt(name, fallback);
}

// Builds a mask of `count` consecutive bits starting at `first`, clamped to the
// width of the mask type.
static uint32_t MakeRangeMask(uint32_t first, uint32_t count)
{
	constexpr uint32_t maskBits = 32;

	if (first >= maskBits)
		return 0;

	count = std::min(count, maskBits - first);

	uint32_t mask = 0;
	for (uint32_t i = 0; i < count; ++i)
		mask |= (0x1u << (first + i));

	return mask;
}

ThreadPinPolicy GetThreadPinPolicy()
{
	// macOS exposes no API to pin a thread to a specific logical processor. The
	// thread_policy_set/THREAD_AFFINITY_POLICY tag is only an advisory hint for
	// grouping threads onto a shared L2, and the scheduler is free to ignore it,
	// so there is no policy we can honour here.
	return THREAD_PIN_POLICY_NONE;
}

ProcessorMasks GetProcessorMasks()
{
	ProcessorMasks processorMasks;

	const int64_t numLogical = GetSysctlInt("hw.logicalcpu", 1);
	const int64_t numPhysical = GetSysctlInt("hw.physicalcpu", numLogical);
	const int64_t numPerfLevels = GetSysctlInt("hw.nperflevels", 1);

	if (numLogical <= 0)
		return processorMasks;

	// Apple Silicon reports one perflevel per core type, ordered fastest-first:
	// perflevel0 is the performance ("P") cluster, perflevel1 the efficiency
	// ("E") cluster. Intel Macs report a single level, so everything there is
	// treated as a performance core.
	if (numPerfLevels >= 2) {
		const int64_t numPerfCpus = GetPerfLevelInt(0, "logicalcpu", numLogical);
		const int64_t numEffCpus = GetPerfLevelInt(1, "logicalcpu", 0);

		processorMasks.performanceCoreMask = MakeRangeMask(0, static_cast<uint32_t>(numPerfCpus));
		processorMasks.efficiencyCoreMask = MakeRangeMask(static_cast<uint32_t>(numPerfCpus), static_cast<uint32_t>(numEffCpus));
	} else {
		processorMasks.performanceCoreMask = MakeRangeMask(0, static_cast<uint32_t>(numLogical));
	}

	// SMT only exists on Intel Macs; Apple Silicon reports logical == physical.
	// Hyperthread siblings are numbered consecutively, so even indices are the
	// "low" thread of each physical core and odd indices the "high" one.
	if (numPhysical > 0 && numLogical > numPhysical) {
		const uint32_t threadsPerCore = static_cast<uint32_t>(numLogical / numPhysical);

		for (uint32_t cpu = 0; cpu < static_cast<uint32_t>(numLogical) && cpu < 32; ++cpu) {
			if ((cpu % threadsPerCore) == 0) {
				processorMasks.hyperThreadLowMask |= (0x1u << cpu);
			} else {
				processorMasks.hyperThreadHighMask |= (0x1u << cpu);
			}
		}
	}

	LOG_L(L_DEBUG, "[CpuTopology::%s] perf=0x%08x eff=0x%08x htLow=0x%08x htHigh=0x%08x",
		__func__,
		processorMasks.performanceCoreMask,
		processorMasks.efficiencyCoreMask,
		processorMasks.hyperThreadLowMask,
		processorMasks.hyperThreadHighMask);

	return processorMasks;
}

ProcessorCaches GetProcessorCache()
{
	ProcessorCaches processorCaches;

	const int64_t numLogical = GetSysctlInt("hw.logicalcpu", 1);
	const int64_t numPerfLevels = GetSysctlInt("hw.nperflevels", 1);

	if (numLogical <= 0)
		return processorCaches;

	// Machine-wide fallbacks, used when the per-perflevel keys are unavailable.
	const int64_t l1dSize = GetSysctlInt("hw.l1dcachesize");
	const int64_t l2Size = GetSysctlInt("hw.l2cachesize");
	const int64_t l3Size = GetSysctlInt("hw.l3cachesize");

	// One group per core cluster: cores within an Apple Silicon cluster share
	// their L2, which is what the caller uses this grouping for.
	const uint32_t numGroups = (numPerfLevels >= 2) ? static_cast<uint32_t>(numPerfLevels) : 1;

	uint32_t firstCpu = 0;

	for (uint32_t level = 0; level < numGroups; ++level) {
		const int64_t groupCpus = (numPerfLevels >= 2)
			? GetPerfLevelInt(level, "logicalcpu", 0)
			: numLogical;

		if (groupCpus <= 0)
			continue;

		ProcessorGroupCaches groupCaches;
		groupCaches.groupMask = MakeRangeMask(firstCpu, static_cast<uint32_t>(groupCpus));
		groupCaches.cacheSizes[0] = static_cast<uint32_t>((numPerfLevels >= 2) ? GetPerfLevelInt(level, "l1dcachesize", l1dSize) : l1dSize);
		groupCaches.cacheSizes[1] = static_cast<uint32_t>((numPerfLevels >= 2) ? GetPerfLevelInt(level, "l2cachesize", l2Size) : l2Size);
		// Apple Silicon has no L3; it exposes a system level cache that is not
		// reported through sysctl, so this stays zero there.
		groupCaches.cacheSizes[2] = static_cast<uint32_t>(l3Size);

		processorCaches.groupCaches.push_back(groupCaches);

		firstCpu += static_cast<uint32_t>(groupCpus);
	}

	// Match the other backends: largest last-level cache first, so that callers
	// pick the performance cluster when they take the first group.
	std::stable_sort(processorCaches.groupCaches.begin(), processorCaches.groupCaches.end(),
		[](const ProcessorGroupCaches& lh, const ProcessorGroupCaches& rh) {
			return (lh.cacheSizes[1] > rh.cacheSizes[1]);
		});

	return processorCaches;
}

} // namespace cpu_topology
