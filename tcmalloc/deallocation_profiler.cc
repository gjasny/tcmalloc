// Copyright 2022 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/deallocation_profiler.h"

#include <algorithm>
#include <cmath>    // for std::lround
#include <cstdint>  // for uintptr_t
#include <functional>
#include <limits>
#include <memory>
#include <string>  // for memset
#include <type_traits>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/internal/low_level_alloc.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/container/flat_hash_map.h"
#include "absl/debugging/stacktrace.h"  // for GetStackTrace
#include "absl/hash/hash.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace deallocationz {
namespace {
using ::absl::base_internal::SpinLock;
using ::absl::base_internal::SpinLockHolder;

// STL adaptor for an arena based allocator which provides the following:
//   static void* Alloc::Allocate(size_t size);
//   static void Alloc::Free(void* ptr, size_t size);
template <typename T, class Alloc>
class AllocAdaptor final {
 public:
  using value_type = T;

  AllocAdaptor() {}
  AllocAdaptor(const AllocAdaptor&) {}

  template <class T1>
  using rebind = AllocAdaptor<T1, Alloc>;

  template <class T1>
  explicit AllocAdaptor(const AllocAdaptor<T1, Alloc>&) {}

  T* allocate(size_t n) {
    // Check if n is too big to allocate.
    ASSERT((n * sizeof(T)) / sizeof(T) == n);
    return static_cast<T*>(Alloc::Allocate(n * sizeof(T)));
  }
  void deallocate(T* p, size_t n) { Alloc::Free(p, n * sizeof(T)); }

  // There's no state, so these allocators are always equal
  bool operator==(const AllocAdaptor&) const { return true; }
  bool operator!=(const AllocAdaptor&) const { return false; }
};

const int64_t kMaxStackDepth = 64;

// Stores stack traces and metadata for any allocation or deallocation
// encountered by the profiler.
struct DeallocationSampleRecord {
  double weight = 0.0;
  size_t requested_size = 0;
  size_t requested_alignment = 0;
  size_t allocated_size = 0;  // size after sizeclass/page rounding

  int depth;  // Number of PC values stored in array below
  void* stack[kMaxStackDepth];

  // creation_time is used to capture the life_time of sampled allocations
  absl::Time creation_time;
  int cpu_id;
  pid_t thread_id;

  template <typename H>
  friend H AbslHashValue(H h, const DeallocationSampleRecord& c) {
    return H::combine(H::combine_contiguous(std::move(h), c.stack, c.depth),
                      c.depth, c.requested_size, c.requested_alignment,
                      c.allocated_size);
  }

  bool operator==(const DeallocationSampleRecord& other) const {
    if (depth != other.depth || requested_size != other.requested_size ||
        requested_alignment != other.requested_alignment ||
        allocated_size != other.allocated_size) {
      return false;
    }
    return std::equal(stack, stack + depth, other.stack);
  }
};

// Tracks whether an object was allocated/deallocated by the same CPU/thread.
struct CpuThreadMatchingStatus {
  constexpr CpuThreadMatchingStatus(bool cpu_matched, bool thread_matched)
      : cpu_matched(cpu_matched),
        thread_matched(thread_matched),
        value((static_cast<int>(cpu_matched) << 1) |
              static_cast<int>(thread_matched)) {}
  bool cpu_matched;
  bool thread_matched;
  int value;
};

struct RpcMatchingStatus {
  static constexpr int ComputeValue(uint64_t alloc, uint64_t dealloc) {
    if (alloc != 0 && dealloc != 0) {
      return static_cast<int>(alloc == dealloc);
    } else {
      return 2;
    }
  }

  constexpr RpcMatchingStatus(uint64_t alloc, uint64_t dealloc)
      : value(ComputeValue(alloc, dealloc)) {}

  int value;
};

int ComputeIndex(CpuThreadMatchingStatus status, RpcMatchingStatus rpc_status) {
  return status.value * 3 + rpc_status.value;
}

constexpr std::pair<CpuThreadMatchingStatus, RpcMatchingStatus> kAllCases[] = {
    {CpuThreadMatchingStatus(false, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, true), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, true), RpcMatchingStatus(0, 0)},

    {CpuThreadMatchingStatus(false, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, true), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, true), RpcMatchingStatus(1, 2)},

    {CpuThreadMatchingStatus(false, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, true), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, true), RpcMatchingStatus(1, 1)},
};
}  // namespace

class DeallocationProfiler {
 private:
  // Arena and allocator used to back STL objects used by DeallocationProfiler
  // Shared between all instances of DeallocationProfiler
  // TODO(b/248332543): Use TCMalloc's own arena allocator instead of defining a
  // new one here. The need for refcount management could be the reason for
  // using a custom allocator in the first place.
  class MyAllocator {
   public:
    static void* Allocate(size_t n) {
      return absl::base_internal::LowLevelAlloc::AllocWithArena(n, arena_);
    }
    static void Free(const void* p, size_t /* n */) {
      absl::base_internal::LowLevelAlloc::Free(const_cast<void*>(p));
    }

    // The lifetime of the arena is managed using a reference count and
    // determined by how long at least one emitted Profile remains alive.
    struct LowLevelArenaReference {
      LowLevelArenaReference() {
        SpinLockHolder h(&arena_lock_);
        if ((refcount_++) == 0) {
          CHECK_CONDITION(arena_ == nullptr);
          arena_ = absl::base_internal::LowLevelAlloc::NewArena(0);
        }
      }

      ~LowLevelArenaReference() {
        SpinLockHolder h(&arena_lock_);
        if ((--refcount_) == 0) {
          CHECK_CONDITION(
              absl::base_internal::LowLevelAlloc::DeleteArena(arena_));
          arena_ = nullptr;
        }
      }
    };

   private:
    // We need to protect the arena with a mutex and ensure that every thread
    // acquires that mutex before it uses the arena for the first time. Once
    // it has acquired the mutex, it is guaranteed that arena won't change
    // between that point in time and when the thread stops accessing it (as
    // enforced by LowLevelArenaReference below).
    ABSL_CONST_INIT static SpinLock arena_lock_;
    static absl::base_internal::LowLevelAlloc::Arena* arena_;

    // We assume that launching a new deallocation profiler takes too long
    // to cause this to overflow within the sampling period. The reason this
    // is not using std::shared_ptr is that we do not only need to protect the
    // value of the reference count but also the pointer itself (and therefore
    // need a separate mutex either way).
    static uint32_t refcount_;
  };

  // This must be the first member of the class to be initialized. The
  // underlying arena must stay alive as long as the profiler.
  MyAllocator::LowLevelArenaReference arena_ref_;

  // All active profilers are stored in a list.
  DeallocationProfiler* next_;
  DeallocationProfilerList* list_ = nullptr;
  friend class DeallocationProfilerList;

  class DeallocationStackTraceTable final
      : public tcmalloc_internal::ProfileBase {
   public:
    // We define the dtor to ensure it is placed in the desired text section.
    ~DeallocationStackTraceTable() override = default;
    void AddTrace(const DeallocationSampleRecord& alloc_trace,
                  const DeallocationSampleRecord& dealloc_trace);

    void Iterate(
        absl::FunctionRef<void(const Profile::Sample&)> func) const override;

    ProfileType Type() const override {
      return tcmalloc::ProfileType::kLifetimes;
    }

    absl::Duration Duration() const override {
      return stop_time_ - start_time_;
    }

    void SetStopTime() { stop_time_ = absl::Now(); }

   private:
    // This must be the first member of the class to be initialized. The
    // underlying arena must stay alive as long as the profile.
    MyAllocator::LowLevelArenaReference arena_ref_;

    static constexpr int kNumCases =
        12;  // CPUthreadMatchingStatus({T,F},{T,F}) x RPCMatchingStatus

    struct Key {
      DeallocationSampleRecord alloc;
      DeallocationSampleRecord dealloc;

      Key(const DeallocationSampleRecord& alloc,
          const DeallocationSampleRecord& dealloc)
          : alloc(alloc), dealloc(dealloc) {}

      template <typename H>
      friend H AbslHashValue(H h, const Key& c) {
        return H::combine(std::move(h), c.alloc, c.dealloc);
      }

      bool operator==(const Key& other) const {
        return (alloc == other.alloc) && (dealloc == other.dealloc);
      }
    };

    struct Value {
      // for each possible cases, we collect repetition count and avg lifetime
      // we also collect the minimum and maximum lifetimes, as well as the sum
      // of squares (to calculate the standard deviation).
      double counts[kNumCases] = {0.0};
      double mean_life_times_ns[kNumCases] = {0.0};
      double variance_life_times_ns[kNumCases] = {0.0};
      double min_life_times_ns[kNumCases] = {0.0};
      double max_life_times_ns[kNumCases] = {0.0};

      Value() {
        std::fill_n(min_life_times_ns, kNumCases,
                    std::numeric_limits<double>::max());
      }
    };

    absl::flat_hash_map<Key, Value, absl::Hash<Key>, std::equal_to<Key>,
                        AllocAdaptor<std::pair<const Key, Value>, MyAllocator>>
        table_;

    absl::Time start_time_ = absl::Now();
    absl::Time stop_time_;
  };

  // Keep track of allocations that are in flight
  absl::flat_hash_map<
      tcmalloc_internal::AllocHandle, DeallocationSampleRecord,
      absl::Hash<tcmalloc_internal::AllocHandle>,
      std::equal_to<tcmalloc_internal::AllocHandle>,
      AllocAdaptor<std::pair<const tcmalloc_internal::AllocHandle,
                             DeallocationSampleRecord>,
                   MyAllocator>>
      allocs_;

  // Table to store lifetime information collected by this profiler
  std::unique_ptr<DeallocationStackTraceTable> reports_ = nullptr;

 public:
  explicit DeallocationProfiler(DeallocationProfilerList* list) : list_(list) {
    reports_ = std::make_unique<DeallocationStackTraceTable>();
    list_->Add(this);
  }

  ~DeallocationProfiler() {
    if (reports_ != nullptr) {
      Stop();
    }
  }

  const tcmalloc::Profile Stop() {
    if (reports_ != nullptr) {
      reports_->SetStopTime();
      list_->Remove(this);
      return tcmalloc_internal::ProfileAccessor::MakeProfile(
          std::move(reports_));
    }
    return tcmalloc::Profile();
  }

  void ReportMalloc(const tcmalloc_internal::StackTrace& stack_trace) {
    // store sampled alloc in the hashmap
    DeallocationSampleRecord& allocation =
        allocs_[stack_trace.sampled_alloc_handle];

    allocation.allocated_size = stack_trace.allocated_size;
    allocation.requested_size = stack_trace.requested_size;
    allocation.requested_alignment = stack_trace.requested_alignment;
    allocation.depth = stack_trace.depth;
    memcpy(allocation.stack, stack_trace.stack,
           sizeof(void*) * std::min(static_cast<int64_t>(stack_trace.depth),
                                    kMaxStackDepth));
    // TODO(mmaas): Do we need to worry about b/65384231 anymore?
    allocation.creation_time = stack_trace.allocation_time;
    allocation.cpu_id = tcmalloc_internal::subtle::percpu::GetCurrentCpu();
    allocation.thread_id = absl::base_internal::GetTID();
    // We divide by the requested size to obtain the number of allocations.
    // TODO(b/248332543): Consider using AllocatedBytes from sampler.h.
    allocation.weight = static_cast<double>(stack_trace.weight) /
                        (stack_trace.requested_size + 1);
  }

  void ReportFree(tcmalloc_internal::AllocHandle handle) {
    auto it = allocs_.find(handle);

    // Handle the case that we observed the deallocation but not the allocation
    if (it == allocs_.end()) {
      return;
    }

    DeallocationSampleRecord sample = it->second;
    allocs_.erase(it);

    DeallocationSampleRecord deallocation;
    deallocation.allocated_size = sample.allocated_size;
    deallocation.requested_alignment = sample.requested_alignment;
    deallocation.requested_size = sample.requested_size;
    deallocation.creation_time = absl::Now();
    deallocation.cpu_id = tcmalloc_internal::subtle::percpu::GetCurrentCpu();
    deallocation.thread_id = absl::base_internal::GetTID();
    deallocation.depth =
        absl::GetStackTrace(deallocation.stack, kMaxStackDepth, 1);

    reports_->AddTrace(sample, deallocation);
  }
};

void DeallocationProfilerList::Add(DeallocationProfiler* profiler) {
  SpinLockHolder h(&profilers_lock_);
  profiler->next_ = first_;
  first_ = profiler;
}

// This list is very short and we're nowhere near a hot path, just walk
void DeallocationProfilerList::Remove(DeallocationProfiler* profiler) {
  SpinLockHolder h(&profilers_lock_);
  DeallocationProfiler** link = &first_;
  DeallocationProfiler* cur = first_;
  while (cur != profiler) {
    CHECK_CONDITION(cur != nullptr);
    link = &cur->next_;
    cur = cur->next_;
  }
  *link = profiler->next_;
}

void DeallocationProfilerList::ReportMalloc(
    const tcmalloc_internal::StackTrace& stack_trace) {
  SpinLockHolder h(&profilers_lock_);
  DeallocationProfiler* cur = first_;
  while (cur != nullptr) {
    cur->ReportMalloc(stack_trace);
    cur = cur->next_;
  }
}

void DeallocationProfilerList::ReportFree(
    tcmalloc_internal::AllocHandle handle) {
  SpinLockHolder h(&profilers_lock_);
  DeallocationProfiler* cur = first_;
  while (cur != nullptr) {
    cur->ReportFree(handle);
    cur = cur->next_;
  }
}

// Initialize static variables
absl::base_internal::LowLevelAlloc::Arena*
    DeallocationProfiler::MyAllocator::arena_ = nullptr;
uint32_t DeallocationProfiler::MyAllocator::refcount_ = 0;
ABSL_CONST_INIT SpinLock DeallocationProfiler::MyAllocator::arena_lock_(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);

void DeallocationProfiler::DeallocationStackTraceTable::AddTrace(
    const DeallocationSampleRecord& alloc_trace,
    const DeallocationSampleRecord& dealloc_trace) {
  CpuThreadMatchingStatus status =
      CpuThreadMatchingStatus(alloc_trace.cpu_id == dealloc_trace.cpu_id,
                              alloc_trace.thread_id == dealloc_trace.thread_id);

  // Initialize a default rpc matched status.
  RpcMatchingStatus rpc_status(/*alloc=*/0, /*dealloc=*/0);

  const int index = ComputeIndex(status, rpc_status);

  DeallocationStackTraceTable::Value& v =
      table_[DeallocationStackTraceTable::Key(alloc_trace, dealloc_trace)];

  const absl::Duration life_time =
      dealloc_trace.creation_time - alloc_trace.creation_time;
  double life_time_ns = absl::ToDoubleNanoseconds(life_time);

  // Update mean and variance using Welford’s online algorithm.
  double old_mean_ns = v.mean_life_times_ns[index];
  v.mean_life_times_ns[index] +=
      (life_time_ns - old_mean_ns) / static_cast<double>(v.counts[index] + 1);
  v.variance_life_times_ns[index] +=
      (life_time_ns - v.mean_life_times_ns[index]) *
      (v.mean_life_times_ns[index] - old_mean_ns);

  v.min_life_times_ns[index] =
      std::min(v.min_life_times_ns[index], life_time_ns);
  v.max_life_times_ns[index] =
      std::max(v.max_life_times_ns[index], life_time_ns);
  v.counts[index]++;
}

void DeallocationProfiler::DeallocationStackTraceTable::Iterate(
    absl::FunctionRef<void(const Profile::Sample&)> func) const {
  uint64_t pair_id = 1;

  for (auto& it : table_) {
    const Key& k = it.first;
    const Value& v = it.second;

    // Report total bytes that are a multiple of the object size.
    size_t allocated_size = k.alloc.allocated_size;

    for (const auto& matching_case : kAllCases) {
      const int index = ComputeIndex(matching_case.first, matching_case.second);
      if (v.counts[index] == 0) {
        continue;
      }

      uintptr_t bytes =
          std::lround(v.counts[index] * k.alloc.weight * allocated_size);
      int64_t count = (bytes + allocated_size - 1) / allocated_size;
      int64_t sum = count * allocated_size;

      // The variance should be >= 0, but it's not impossible that it drops
      // below 0 for numerical reasons. We don't want to crash in this case,
      // so we ensure to return 0 if this happens.
      double stddev_life_time_ns =
          sqrt(std::max(0.0, v.variance_life_times_ns[index] /
                                 static_cast<double>((v.counts[index]))));

      const auto bucketize_ns = internal::LifetimeToBucketedLifetimeNanoseconds;
      Profile::Sample sample{
          .sum = sum,
          .requested_size = k.alloc.requested_size,
          .requested_alignment = k.alloc.requested_alignment,
          .allocated_size = allocated_size,
          .profile_id = pair_id,
          .lifetime_ns = bucketize_ns(v.mean_life_times_ns[index]),
          .stddev_lifetime_ns = bucketize_ns(stddev_life_time_ns),
          .min_lifetime_ns = bucketize_ns(v.min_life_times_ns[index]),
          .max_lifetime_ns = bucketize_ns(v.max_life_times_ns[index]),
          .allocator_deallocator_cpu_matched = matching_case.first.cpu_matched,
          .allocator_deallocator_thread_matched =
              matching_case.first.thread_matched,
      };

      // first for allocation
      sample.count = count;
      sample.depth = k.alloc.depth;
      std::copy(k.alloc.stack, k.alloc.stack + k.alloc.depth, sample.stack);
      func(sample);

      // second for deallocation
      static_assert(
          std::is_signed<decltype(tcmalloc::Profile::Sample::count)>::value,
          "Deallocation samples are tagged with negative count values.");
      sample.count = -1 * count;
      sample.depth = k.dealloc.depth;
      std::copy(k.dealloc.stack, k.dealloc.stack + k.dealloc.depth,
                sample.stack);
      func(sample);

      pair_id++;
    }
  }
}

DeallocationSample::DeallocationSample(DeallocationProfilerList* list) {
  profiler_ = std::make_unique<DeallocationProfiler>(list);
}

tcmalloc::Profile DeallocationSample::Stop() && {
  if (profiler_ != nullptr) {
    tcmalloc::Profile profile = profiler_->Stop();
    profiler_.reset();
    return profile;
  }
  return tcmalloc::Profile();
}

namespace internal {

// Lifetimes below 1ns are truncated to 1ns.  Lifetimes between 1ns and 1ms
// are rounded to the next smaller power of 10.  Lifetimes above 1ms are rounded
// down to the nearest millisecond.
uintptr_t LifetimeToBucketedLifetimeNanoseconds(double lifetime_ns) {
  if (lifetime_ns < 1000000.0) {
    if (lifetime_ns <= 1) {
      // Avoid negatives.  We can't allocate in a negative amount of time or
      // even as quickly as a nanosecond (microbenchmarks of
      // allocation/deallocation in a tight loop are several nanoseconds), so
      // results this small indicate probable clock skew or other confounding
      // factors in the data.
      return 1;
    }

    for (uintptr_t cutoff_ns = 10; cutoff_ns <= 1000000; cutoff_ns *= 10) {
      if (lifetime_ns < cutoff_ns) {
        return cutoff_ns / 10;
      }
    }
  }

  // Round down to nearest millisecond.
  return static_cast<uintptr_t>(lifetime_ns / 1000000.0) * 1000000L;
}

}  // namespace internal
}  // namespace deallocationz
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END