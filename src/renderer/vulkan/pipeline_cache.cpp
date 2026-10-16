#include "pipeline_cache.h"

#include <fstream>
#include <iostream>

#include <rex/logging.h>

namespace fable2::renderer::vulkan {

namespace {
constexpr uint32_t kMagic = 0x636b5666u;  // 'fVkc' little-endian
constexpr uint32_t kVersion = 1u;

struct ReadView {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;

  bool ReadBytes(void* out, size_t n) {
    if (pos + n > size) return false;
    std::memcpy(out, data + pos, n);
    pos += n;
    return true;
  }
  bool ReadU32(uint32_t& out) { return ReadBytes(&out, sizeof(out)); }
  bool ReadU64(uint64_t& out) { return ReadBytes(&out, sizeof(out)); }
};
}  // namespace

PipelineCache::PipelineCache(Functions& fn, VkDevice device) : fn_(fn), device_(device) {
  VkPipelineCacheCreateInfo createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  createInfo.initialDataSize = 0;
  createInfo.pInitialData = nullptr;
  if (fn_.vkCreatePipelineCache &&
      fn_.vkCreatePipelineCache(device_, &createInfo, nullptr, &cache_) != VK_SUCCESS) {
    REXGPU_WARN("fable2-renderer: failed to create VkPipelineCache");
    cache_ = VK_NULL_HANDLE;
  }
}

PipelineCache::~PipelineCache() {
  if (cache_ && fn_.vkDestroyPipelineCache) {
    fn_.vkDestroyPipelineCache(device_, cache_, nullptr);
  }
}

bool PipelineCache::LoadFromDisk(const std::string& path, std::string& err) {
  lastPath_ = path;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err.clear();
    return false;  // no cache yet is not an error
  }
  file.seekg(0, std::ios::end);
  const std::streamoff len = file.tellg();
  file.seekg(0, std::ios::beg);
  if (len <= 0) return false;
  std::vector<uint8_t> raw(static_cast<size_t>(len));
  if (!file.read(reinterpret_cast<char*>(raw.data()), len)) {
    err = "failed to read pipeline cache: " + path;
    return false;
  }

  ReadView view{raw.data(), raw.size(), 0};
  uint32_t magic = 0, version = 0, blobSize = 0;
  if (!view.ReadU32(magic) || magic != kMagic || !view.ReadU32(version) || version != kVersion ||
      !view.ReadU32(blobSize) || view.pos + blobSize > view.size) {
    err = "pipeline cache: unrecognized format: " + path;
    return false;
  }
  const uint8_t* blob = view.data + view.pos;
  view.pos += blobSize;

  entries_.clear();
  uint32_t entryCount = 0;
  if (!view.ReadU32(entryCount)) {
    err = "pipeline cache: truncated header: " + path;
    return false;
  }
  for (uint32_t i = 0; i < entryCount; ++i) {
    uint32_t keyLen = 0, dataLen = 0;
    // File order: keyLen, key, dataLen, data.
    if (!view.ReadU32(keyLen) || view.pos + keyLen > view.size) {
      err = "pipeline cache: truncated entry table: " + path;
      return false;
    }
    std::string key(reinterpret_cast<const char*>(view.data + view.pos), keyLen);
    view.pos += keyLen;
    if (!view.ReadU32(dataLen) || view.pos + dataLen > view.size) {
      err = "pipeline cache: truncated entry table: " + path;
      return false;
    }
    std::vector<uint8_t> data(dataLen);
    if (dataLen) {
      std::memcpy(data.data(), view.data + view.pos, dataLen);
      view.pos += dataLen;
    }
    entries_.emplace(std::move(key), std::move(data));
  }

  // (Re)build the live VkPipelineCache from the loaded blob.
  if (cache_ && fn_.vkDestroyPipelineCache) {
    fn_.vkDestroyPipelineCache(device_, cache_, nullptr);
    cache_ = VK_NULL_HANDLE;
  }
  VkPipelineCacheCreateInfo createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  createInfo.initialDataSize = blobSize;
  createInfo.pInitialData = blob;
  if (fn_.vkCreatePipelineCache &&
      fn_.vkCreatePipelineCache(device_, &createInfo, nullptr, &cache_) != VK_SUCCESS) {
    err = "failed to re-create VkPipelineCache from disk image";
    return false;
  }
  REXGPU_INFO("fable2-renderer: loaded pipeline cache ({} bytes, {} keyed entries) from {}",
              blobSize, entries_.size(), path);
  return true;
}

bool PipelineCache::SaveToDisk(const std::string& path, std::string& err) const {
  if (!cache_ || !fn_.vkGetPipelineCacheData) {
    err = "no live VkPipelineCache to save";
    return false;
  }
  // Query the current cache image. (vkGetPipelineCacheData uses size_t*.)
  size_t dataSize = 0;
  fn_.vkGetPipelineCacheData(device_, cache_, &dataSize, nullptr);
  std::vector<uint8_t> blob(dataSize);
  if (dataSize &&
      fn_.vkGetPipelineCacheData(device_, cache_, &dataSize, blob.data()) != VK_SUCCESS) {
    err = "vkGetPipelineCacheData failed";
    return false;
  }
  const uint32_t dataSize32 = static_cast<uint32_t>(dataSize);

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    err = "failed to open for writing: " + path;
    return false;
  }
  uint32_t magic = kMagic, version = kVersion;
  file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
  file.write(reinterpret_cast<const char*>(&version), sizeof(version));
  file.write(reinterpret_cast<const char*>(&dataSize32), sizeof(dataSize32));
  if (dataSize) {
    file.write(reinterpret_cast<const char*>(blob.data()), dataSize);
  }
  const uint32_t entryCount = static_cast<uint32_t>(entries_.size());
  file.write(reinterpret_cast<const char*>(&entryCount), sizeof(entryCount));
  for (const auto& kv : entries_) {
    const uint32_t keyLen = static_cast<uint32_t>(kv.first.size());
    const uint32_t dataLen = static_cast<uint32_t>(kv.second.size());
    file.write(reinterpret_cast<const char*>(&keyLen), sizeof(keyLen));
    file.write(kv.first.data(), keyLen);
    file.write(reinterpret_cast<const char*>(&dataLen), sizeof(dataLen));
    if (dataLen) file.write(reinterpret_cast<const char*>(kv.second.data()), dataLen);
  }
  file.flush();
  if (!file) {
    err = "failed to write pipeline cache: " + path;
    return false;
  }
  REXGPU_INFO("fable2-renderer: saved pipeline cache ({} bytes, {} keyed entries) to {}",
              dataSize, entries_.size(), path);
  return true;
}

void PipelineCache::StoreEntry(const std::string& key, const void* data, size_t size) {
  entries_[key] = std::vector<uint8_t>(static_cast<const uint8_t*>(data),
                                       static_cast<const uint8_t*>(data) + size);
}

const std::vector<uint8_t>* PipelineCache::LookupEntry(const std::string& key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    missCount_ += 1;
    return nullptr;
  }
  hitCount_ += 1;
  return &it->second;
}

PipelineCache::Stats PipelineCache::GetStats() const {
  Stats s;
  if (cache_ && fn_.vkGetPipelineCacheData) {
    size_t blobSize = 0;
    fn_.vkGetPipelineCacheData(device_, cache_, &blobSize, nullptr);
    s.diskCacheBytes = static_cast<uint32_t>(blobSize);
    s.hasDiskCache = blobSize > 0;
  }
  s.entryCount = static_cast<uint32_t>(entries_.size());
  s.hitCount = hitCount_;
  s.missCount = missCount_;
  return s;
}

}  // namespace fable2::renderer::vulkan
