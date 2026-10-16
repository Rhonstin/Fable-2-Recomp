#pragma once
//
// fable2 :: renderer :: vulkan :: MemoryTracker
//
// Centralised GPU-memory accounting for the custom renderer. Every allocation
// made by the renderer goes through one of the tracked pools so that:
//   * per-pool (device-local / host-visible / frame / upload / persistent)
//     residency is always known,
//   * the per-frame transient peak is measured (this is the "frame budget"), and
//   * a hard budget can be enforced with graceful degradation instead of the
//     old ad-hoc `new`-everything behaviour.
//
#include <cstdint>
#include <string>
#include <unordered_map>

namespace fable2::renderer::vulkan {

class MemoryTracker {
 public:
  struct Pool {
    std::string name;
    uint64_t residentBytes = 0;  // currently held by this pool
    uint64_t peakResidentBytes = 0;
    uint64_t peakFrameBytes = 0;  // only meaningful for the frame pool
    uint64_t allocationCount = 0;
    uint64_t totalBytesAllocated = 0;  // lifetime
    bool frameScoped = false;  // freed at the end of every frame
  };

  MemoryTracker() = default;
  explicit MemoryTracker(uint64_t totalBudgetBytes) { SetBudget(totalBudgetBytes); }

  // The hard budget: the maximum total resident GPU memory the renderer will
  // use before it starts degrading (dropping frame-scoped work, evicting
  // LRU cache entries, etc.). 0 == no budget (report only).
  void SetBudget(uint64_t totalBudgetBytes) { budgetBytes_ = totalBudgetBytes; }
  uint64_t Budget() const { return budgetBytes_; }
  bool BudgetTripped() const { return budgetTripped_; }
  void ClearBudgetTripped() { budgetTripped_ = false; }

  // --- frame lifecycle ---
  void BeginFrame();
  void EndFrame();

  // --- allocation accounting ---
  // Allocate records `bytes` into the named pool (creating it on first use).
  // Returns a reference to the pool. The caller is responsible for pairing
  // with Free() exactly once.
  Pool& Allocate(const std::string& poolName, uint64_t bytes);
  void Free(Pool& pool, uint64_t bytes);

  // Convenience for the standard pools.
  Pool& GetPool(const char* name);
  Pool& DeviceLocalPool() { return GetPool("device-local"); }
  Pool& HostVisiblePool() { return GetPool("host-visible"); }
  Pool& FramePool() { return GetPool("frame"); }
  Pool& UploadPool() { return GetPool("upload"); }
  Pool& PersistentPool() { return GetPool("persistent"); }

  // --- queries ---
  uint64_t TotalResidentBytes() const;  // sum across all pools
  uint64_t PeakFrameBytes() const { return peakFrameBytes_; }
  uint64_t PoolCount() const { return pools_.size(); }
  const Pool* Find(const std::string& name) const;
  const std::unordered_map<std::string, Pool>& Pools() const { return pools_; }

  // Human-readable summary for diagnostics (log / validation tooling).
  std::string Describe() const;

 private:
  Pool& GetPoolImpl(const std::string& name, bool frameScoped);
  void UpdateTotals();

  uint64_t budgetBytes_ = 0;
  bool budgetTripped_ = false;
  uint64_t totalResident_ = 0;
  uint64_t peakFrameBytes_ = 0;
  std::unordered_map<std::string, Pool> pools_;
};

}  // namespace fable2::renderer::vulkan
