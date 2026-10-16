#pragma once
//
// fable2 :: renderer :: vulkan :: PipelineCache
//
// The renderer's pipeline cache. It owns an in-memory VkPipelineCache (the
// canonical Vulkan mechanism: a device keeps a hash-keyed image of pipeline
// data) and adds:
//   * an on-disk blob (dump/load at startup) so the first run after a build
//     does not pay the full pipeline-compilation cost, and
//   * a keyed sidecar store so the renderer can associate a stable key
//     (hash of shader + descriptor layout + render pass) with cached data
//     across runs.
//
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "vk_function_table.h"

namespace fable2::renderer::vulkan {

class PipelineCache {
 public:
  struct Stats {
    bool hasDiskCache = false;  // a disk cache was loaded at startup
    uint32_t diskCacheBytes = 0;
    uint32_t entryCount = 0;
    uint64_t hitCount = 0;
    uint64_t missCount = 0;
  };

  PipelineCache(Functions& fn, VkDevice device);
  ~PipelineCache();
  PipelineCache(const PipelineCache&) = delete;
  PipelineCache& operator=(const PipelineCache&) = delete;

  VkPipelineCache handle() const { return cache_; }
  const std::string& LastPath() const { return lastPath_; }

  // Load the on-disk cache (if the file exists) into the live VkPipelineCache.
  // Returns true if a non-empty disk cache was loaded.
  bool LoadFromDisk(const std::string& path, std::string& err);
  // Dump the current in-memory cache + keyed entries to disk.
  bool SaveToDisk(const std::string& path, std::string& err) const;

  // Keyed sidecar store (the renderer's own pipeline -> data association).
  void StoreEntry(const std::string& key, const void* data, size_t size);
  const std::vector<uint8_t>* LookupEntry(const std::string& key);

  Stats GetStats() const;

 private:
  Functions& fn_;
  VkDevice device_;
  VkPipelineCache cache_ = VK_NULL_HANDLE;
  std::string lastPath_;
  std::unordered_map<std::string, std::vector<uint8_t>> entries_;
  uint64_t hitCount_ = 0, missCount_ = 0;
};

}  // namespace fable2::renderer::vulkan
