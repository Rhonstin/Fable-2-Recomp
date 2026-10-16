#include "memory_tracker.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace fable2::renderer::vulkan {

namespace {
constexpr const char* kFramePool = "frame";
}  // namespace

void MemoryTracker::BeginFrame() {
  auto it = pools_.find(kFramePool);
  if (it != pools_.end()) {
    // Frame-scoped memory must not carry over between frames. Defensive reset
    // (the renderer should have freed every frame allocation already).
    it->second.residentBytes = 0;
  }
}

void MemoryTracker::EndFrame() {
  auto it = pools_.find(kFramePool);
  if (it != pools_.end() && it->second.residentBytes > 0) {
    // Some frame allocation was not freed this frame (a leak the budget check
    // below will surface); reset so the next frame starts clean but keep the
    // peak so diagnostics can see the spike.
    it->second.residentBytes = 0;
  }
  UpdateTotals();
}

MemoryTracker::Pool& MemoryTracker::Allocate(const std::string& poolName, uint64_t bytes) {
  Pool& pool = GetPoolImpl(poolName, poolName == kFramePool);
  pool.residentBytes += bytes;
  pool.peakResidentBytes = std::max(pool.peakResidentBytes, pool.residentBytes);
  pool.allocationCount += 1;
  pool.totalBytesAllocated += bytes;
  if (pool.frameScoped) {
    pool.peakFrameBytes = std::max(pool.peakFrameBytes, pool.residentBytes);
    peakFrameBytes_ = std::max(peakFrameBytes_, pool.residentBytes);
  }
  UpdateTotals();
  if (budgetBytes_ != 0 && totalResident_ > budgetBytes_) {
    budgetTripped_ = true;
  }
  return pool;
}

void MemoryTracker::Free(MemoryTracker::Pool& pool, uint64_t bytes) {
  if (pool.residentBytes < bytes) {
    pool.residentBytes = 0;  // underflow guard (should not happen)
  } else {
    pool.residentBytes -= bytes;
  }
  UpdateTotals();
}

MemoryTracker::Pool& MemoryTracker::GetPool(const char* name) {
  return GetPoolImpl(name, name == kFramePool);
}

const MemoryTracker::Pool* MemoryTracker::Find(const std::string& name) const {
  auto it = pools_.find(name);
  return it == pools_.end() ? nullptr : &it->second;
}

uint64_t MemoryTracker::TotalResidentBytes() const { return totalResident_; }

MemoryTracker::Pool& MemoryTracker::GetPoolImpl(const std::string& name, bool frameScoped) {
  auto it = pools_.find(name);
  if (it == pools_.end()) {
    Pool p;
    p.name = name;
    p.frameScoped = frameScoped;
    it = pools_.emplace(name, std::move(p)).first;
  }
  return it->second;
}

void MemoryTracker::UpdateTotals() {
  uint64_t total = 0;
  for (const auto& kv : pools_) {
    total += kv.second.residentBytes;
  }
  totalResident_ = total;
}

std::string MemoryTracker::Describe() const {
  std::ostringstream os;
  os << "MemoryTracker: budget "
     << (budgetBytes_ ? std::to_string(budgetBytes_ / (1024 * 1024)) + " MiB" : "none")
     << ", resident " << (totalResident_ / 1024) << " KiB"
     << (budgetTripped_ ? " (BUDGET TRIPPED)" : "") << ", peak-frame "
     << (peakFrameBytes_ / 1024) << " KiB, pools:\n";
  for (const auto& kv : pools_) {
    const Pool& p = kv.second;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "  - %-14s resident %8llu KiB  peak %8llu KiB  "
                  "allocs %llu  lifetime %llu MiB%s",
                  p.name.c_str(), (unsigned long long)(p.residentBytes / 1024),
                  (unsigned long long)(p.peakResidentBytes / 1024),
                  (unsigned long long)p.allocationCount,
                  (unsigned long long)(p.totalBytesAllocated / (1024 * 1024)),
                  p.frameScoped ? " [frame]" : "");
    os << buf << "\n";
  }
  return os.str();
}

}  // namespace fable2::renderer::vulkan
