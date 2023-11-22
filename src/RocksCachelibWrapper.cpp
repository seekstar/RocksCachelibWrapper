/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "RocksCachelibWrapper.h"

#include "folly/init/Init.h"
#include "folly/synchronization/Rcu.h"
#include "rocksdb/version.h"

namespace facebook::rocks_secondary_cache {

#define FB_CACHE_MAX_ITEM_SIZE 4 << 20

namespace {
// We use a separate RCU domain since read side critical sections can block
// on IO, and we don't want to interfere with other system activities that
// use RCU synchronization.
folly::rcu_domain& GetRcuDomain() {
  static folly::rcu_domain domain;
  return domain;
}

class RocksCachelibWrapperHandle : public rocksdb::SecondaryCacheResultHandle {
 public:
  RocksCachelibWrapperHandle(folly::SemiFuture<FbCacheReadHandle>&& future,
                             const rocksdb::Cache::CacheItemHelper* helper,
                             rocksdb::Cache::CreateContext* create_context,
                             std::unique_lock<folly::rcu_domain>&& guard)
      : future_(std::move(future)),
        helper_(helper),
        create_context_(create_context),
        val_(nullptr),
        charge_(0),
        is_value_ready_(false),
        guard_(std::move(guard)) {}
  ~RocksCachelibWrapperHandle() override = default;

  RocksCachelibWrapperHandle(const RocksCachelibWrapperHandle&) = delete;
  RocksCachelibWrapperHandle& operator=(const RocksCachelibWrapperHandle&) =
      delete;

  bool IsReady() override {
    bool ready = true;
    if (!is_value_ready_) {
      ready = future_.isReady();
      if (ready) {
        handle_ = std::move(future_).value();
        CalcValue();
      }
    }
    return ready;
  }

  void Wait() override {
    if (!is_value_ready_) {
      future_.wait();
      handle_ = std::move(future_).value();
      CalcValue();
    }
  }

  static void WaitAll(
      std::vector<rocksdb::SecondaryCacheResultHandle*> handles) {
    std::vector<folly::SemiFuture<FbCacheReadHandle>> h_semi;
    for (auto h_ptr : handles) {
      RocksCachelibWrapperHandle* hdl =
          static_cast<RocksCachelibWrapperHandle*>(h_ptr);
      if (hdl->is_value_ready_) {
        continue;
      }
      h_semi.emplace_back(std::move(hdl->future_));
    }
    auto all_handles = folly::collectAll(std::move(h_semi));
    auto new_handles = std::move(all_handles).get();
    assert(new_handles.size() == h_semi.size());
    int result_idx = 0;
    for (size_t i = 0; i < handles.size(); ++i) {
      RocksCachelibWrapperHandle* hdl =
          static_cast<RocksCachelibWrapperHandle*>(handles[i]);
      if (hdl->is_value_ready_) {
        continue;
      }
      hdl->handle_ = std::move(new_handles[result_idx]).value();
      result_idx++;
      hdl->CalcValue();
    }
  }

  void* Value() override { return val_; }

  size_t Size() override { return charge_; }

 private:
  FbCacheReadHandle handle_;
  folly::SemiFuture<FbCacheReadHandle> future_;
  const rocksdb::Cache::CacheItemHelper* const helper_;
  rocksdb::Cache::CreateContext* const create_context_;
  void* val_;
  size_t charge_;
  bool is_value_ready_;
  std::unique_lock<folly::rcu_domain> guard_;

  void CalcValue() {
    is_value_ready_ = true;

    if (handle_) {
      uint32_t size = handle_->getSize();
      rocksdb::Status s;

      const char* item = static_cast<const char*>(handle_->getMemory());
      s = helper_->create_cb(rocksdb::Slice(item, size),
#if ROCKSDB_MAJOR > 8 || (ROCKSDB_MAJOR == 8 && ROCKSDB_MINOR >= 7)
                             rocksdb::CompressionType::kNoCompression,
                             rocksdb::CacheTier::kVolatileTier,
#endif
                             create_context_,
                             /*allocator*/ nullptr,
                             &val_,
                             &charge_);
      if (!s.ok()) {
        val_ = nullptr;
      }
      handle_.reset();
    }
  }
};
} // namespace

RocksCachelibWrapper::~RocksCachelibWrapper() { Close(); }

rocksdb::Status RocksCachelibWrapper::Insert(
    const rocksdb::Slice& key,
    void* value,
    const rocksdb::Cache::CacheItemHelper* helper,
    bool /* force_erase */) {
  FbCacheKey k(key.data(), key.size());
  size_t size;
  rocksdb::Status s;
  std::scoped_lock guard(GetRcuDomain());
  FbCache* cache = cache_.load();

  if (cache) {
    size = (*helper->size_cb)(value);
    if (FbCacheItem::getRequiredSize(k, size) <= FB_CACHE_MAX_ITEM_SIZE) {
      auto handle = cache->allocate(pool_, k, size);
      if (handle) {
        char* buf = static_cast<char*>(handle->getMemory());
        s = (*helper->saveto_cb)(value, /*offset=*/0, size, buf);
        try {
          cache->insertOrReplace(handle);
        } catch (const std::exception& ex) {
          s = rocksdb::Status::Aborted(folly::sformat(
              "Cachelib insertOrReplace exception, error:{}", ex.what()));
        }
      }
    } else {
      s = rocksdb::Status::InvalidArgument();
    }
  }
  return s;
}

std::unique_ptr<rocksdb::SecondaryCacheResultHandle>
RocksCachelibWrapper::Lookup(const rocksdb::Slice& key,
                             const rocksdb::Cache::CacheItemHelper* helper,
                             rocksdb::Cache::CreateContext* create_context,
                             bool wait,
                             bool /*advise_erase*/,
                             bool& is_in_sec_cache) {
  std::unique_lock guard(GetRcuDomain());
  FbCache* cache = cache_.load();
  std::unique_ptr<rocksdb::SecondaryCacheResultHandle> hdl;

  if (cache) {
    auto handle = cache->find(FbCacheKey(key.data(), key.size()));
    // We cannot dereference the handle in anyway. Any dereference will make it
    // synchronous, so get the SamiFuture right away
    // std::move the std::unique_lock<rcu_domain> (reader lock) to the
    // RocksCachelibWrapperHandle, and will be released when the handle is
    // destroyed.
    hdl = std::make_unique<RocksCachelibWrapperHandle>(
        std::move(handle).toSemiFuture(),
        helper,
        create_context,
        std::move(guard));
    if (hdl->IsReady() || wait) {
      if (!hdl->IsReady()) { // WART: double-call IsReady()
        hdl->Wait();
      }
      if (hdl->Value() == nullptr) {
        hdl.reset();
      }
    }
  }

  is_in_sec_cache = hdl != nullptr;
  return hdl;
}

void RocksCachelibWrapper::Erase(const rocksdb::Slice& key) {
  std::scoped_lock guard(GetRcuDomain());
  FbCache* cache = cache_.load();

  if (cache) {
    cache->remove(FbCacheKey(key.data(), key.size()));
  }
}

void RocksCachelibWrapper::WaitAll(
    std::vector<rocksdb::SecondaryCacheResultHandle*> handles) {
  RocksCachelibWrapperHandle::WaitAll(handles);
}

void RocksCachelibWrapper::Close() {
  FbCache* cache = cache_.load();
  if (cache) {
    // Nullify the cache pointer, then wait for all read side critical
    // sections already started to finish, and then delete the cache
    cache_.store(nullptr);
    GetRcuDomain().synchronize();
    delete cache;
  }
}

// Global cache object and a default cache pool
std::unique_ptr<rocksdb::SecondaryCache> NewRocksCachelibWrapper(
    const RocksCachelibOptions& opts) {
  std::unique_ptr<FbCache> cache;
  cachelib::PoolId defaultPool;
  FbCacheConfig config;
  NvmCacheConfig nvmConfig;

  nvmConfig.navyConfig.setBlockSize(opts.blockSize);
  nvmConfig.navyConfig.setSimpleFile(opts.fileName,
                                     opts.size,
                                     /*truncateFile=*/true);
  nvmConfig.navyConfig.blockCache().setRegionSize(opts.regionSize);
  if (opts.admPolicy == "random") {
    nvmConfig.navyConfig.enableRandomAdmPolicy().setAdmProbability(
        opts.admProbability);
  } else {
    nvmConfig.navyConfig.enableDynamicRandomAdmPolicy()
        .setMaxWriteRate(opts.maxWriteRate)
        .setAdmWriteRate(opts.admissionWriteRate);
  }
  nvmConfig.enableFastNegativeLookups = true;

  config.setCacheSize(opts.volatileSize)
      .setCacheName(opts.cacheName)
      .setAccessConfig(
          {opts.bktPower /* bucket power */, opts.lockPower /* lock power */})
      .enableNvmCache(nvmConfig)
      .validate(); // will throw if bad config
  cache = std::make_unique<FbCache>(config);
  defaultPool =
      cache->addPool("default", cache->getCacheMemoryStats().ramCacheSize);

  return std::unique_ptr<rocksdb::SecondaryCache>(new RocksCachelibWrapper(
      std::move(cache), std::move(defaultPool)));
}

} // namespace facebook::rocks_secondary_cache
