#ifndef KYTY_TILED_DOWNLOAD_COUNTS_DIAGNOSTIC_H_
#define KYTY_TILED_DOWNLOAD_COUNTS_DIAGNOSTIC_H_

// Temporary default-OFF observer. No Vulkan objects, queries, callbacks, timers,
// guest reads, resource queries, ownership changes, allocations or per-event I/O.
// Admission and reporting are owned by the existing single LabelCausal epoch.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace Libs::Graphics::TiledDownloadDiagnostic {
inline constexpr size_t BucketCapacity = 64;
inline constexpr size_t FieldCount = 33;
using Key = std::array<uint64_t, FieldCount>;
inline constexpr std::array<const char*, FieldCount> FieldNames {
    "width", "height", "depth", "bpe", "family", "tail", "surface_z",
    "tile_width", "tile_height", "tile_depth", "regions", "tiles", "transform",
    "guest_tile", "guest_format", "native_format", "native_flags", "native_usage",
    "samples", "levels", "layers", "native_type", "metadata", "stencil", "block",
    "depth_image", "raw_sizeclass_compatible", "max_push_descriptors", "copy_aspect",
    "attachment_layout_not_current_layout", "row_length", "buffer_image_height",
    "mixed_tile_shapes"};
struct Bucket {
  // 0 free, 1 being initialized, 2 immutable key published. Atomic counters
  // permit safe incomplete snapshots; no observer locks or spin waits.
  std::atomic<uint32_t> state {0};
  Key key {};
  std::atomic<uint64_t> calls {0}, linear_bytes {0}, tiled_capacity_bytes {0};
  std::atomic<uint64_t> first_address {0};
};
inline std::array<Bucket, BucketCapacity> buckets {};
inline std::atomic<uint64_t> calls {0}, linear_bytes {0}, tiled_capacity_bytes {0};
inline std::atomic<uint64_t> overflow_calls {0}, overflow_linear_bytes {0};
inline std::atomic<uint64_t> first_frame {UINT64_MAX}, last_frame {0};

inline void Add(const Key& key, uint64_t linear, uint64_t capacity,
                uint64_t address, uint64_t frame) noexcept {
  calls.fetch_add(1, std::memory_order_relaxed);
  linear_bytes.fetch_add(linear, std::memory_order_relaxed);
  tiled_capacity_bytes.fetch_add(capacity, std::memory_order_relaxed);
  auto unobserved = UINT64_MAX;
  first_frame.compare_exchange_strong(unobserved, frame, std::memory_order_relaxed);
  last_frame.store(frame, std::memory_order_relaxed);
  for (auto& bucket : buckets) {
    auto state = bucket.state.load(std::memory_order_acquire);
    if (state == 0 && bucket.state.compare_exchange_strong(state, 1,
        std::memory_order_acquire, std::memory_order_relaxed)) {
      bucket.key = key;
      bucket.first_address.store(address, std::memory_order_relaxed);
      bucket.state.store(2, std::memory_order_release);
      state = 2;
    } else if (state != 2) {
      // A failed claim may have observed an initialized key: acquire publication
      // explicitly before reading it. Never wait for an in-progress claim.
      state = bucket.state.load(std::memory_order_acquire);
    }
    if (state == 2 && bucket.key == key) {
      bucket.calls.fetch_add(1, std::memory_order_relaxed);
      bucket.linear_bytes.fetch_add(linear, std::memory_order_relaxed);
      bucket.tiled_capacity_bytes.fetch_add(capacity, std::memory_order_relaxed);
      return;
    }
  }
  overflow_calls.fetch_add(1, std::memory_order_relaxed);
  overflow_linear_bytes.fetch_add(linear, std::memory_order_relaxed);
}

inline void Report() noexcept {
  const auto total_calls = calls.load(std::memory_order_relaxed);
  const auto first = first_frame.load(std::memory_order_relaxed);
  std::printf("tiled-download-counts population=successful_tileimage_recordings_in_label_epoch "
      "gpu_completed=unobserved format_read_caps=unqueried timestamp_queries=0 "
      "snapshot=atomic_fields_not_transactional capacity=%zu calls=%llu "
      "linear_bytes_argument=%llu tiled_capacity_argument=%llu first_frame=%llu "
      "last_frame=%llu overflow_calls=%llu overflow_linear_bytes=%llu\n",
      BucketCapacity, static_cast<unsigned long long>(total_calls),
      static_cast<unsigned long long>(linear_bytes.load()),
      static_cast<unsigned long long>(tiled_capacity_bytes.load()),
      static_cast<unsigned long long>(first == UINT64_MAX ? 0 : first),
      static_cast<unsigned long long>(last_frame.load()),
      static_cast<unsigned long long>(overflow_calls.load()),
      static_cast<unsigned long long>(overflow_linear_bytes.load()));
  for (size_t index = 0; index < buckets.size(); ++index) {
    const auto& bucket = buckets[index];
    if (bucket.state.load(std::memory_order_acquire) != 2) continue;
    const auto count = bucket.calls.load(std::memory_order_relaxed);
    if (count == 0) continue;
    std::printf("tiled-download-bucket slot=%zu calls=%llu linear_bytes_argument=%llu "
        "tiled_capacity_argument=%llu example_address=0x%llx", index,
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(bucket.linear_bytes.load()),
        static_cast<unsigned long long>(bucket.tiled_capacity_bytes.load()),
        static_cast<unsigned long long>(bucket.first_address.load()));
    for (size_t field = 0; field < FieldCount; ++field)
      std::printf(" %s=%llu", FieldNames[field],
          static_cast<unsigned long long>(bucket.key[field]));
    std::printf("\n");
  }
}
} // namespace Libs::Graphics::TiledDownloadDiagnostic
#endif
