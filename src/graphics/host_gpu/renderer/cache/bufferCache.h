#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class ReadbackQueue;

// Waits until the uploads handed to the helper thread are in the staging buffer (see
// BufferCache::UploadCopies). Called before recorded commands are submitted to the GPU.
void JoinAsyncUploads() noexcept;

struct GraphicContext;
class CommandScheduler;
class TextureCache;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

// Carry the actual access origin independently of optional diagnostics.
enum class MemoryReadOrigin : uint8_t { Explicit, Fault, WriteFault };

class BufferCache {
public:
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	// The entry after the last page holds the zero page's device address: shaders read it once and
	// load from the zero page where a page is not mapped, instead of branching around the load.
	// Unmapped pages keep entry 0, so stores and atomics still see them as not mapped.
	static constexpr uint64_t BDA_ZERO_PAGE_ENTRY = CACHING_NUMPAGES;
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    (CACHING_NUMPAGES + 1) * sizeof(vk::DeviceAddress);

	static constexpr uint64_t PageIndex(uint64_t address) {
		return (address < LOWER_ADDRESS_SIZE
		            ? address
		            : address - LibKernel::Memory::kExtendedMemoryBase + LOWER_ADDRESS_SIZE) >>
		       CACHING_PAGEBITS;
	}
	static constexpr uint64_t GuestAddress(uint64_t offset) {
		return offset < LOWER_ADDRESS_SIZE
		           ? offset
		           : offset - LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemoryBase;
	}

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                   InvalidateMemory(uint64_t vaddr, uint64_t size,
	                                        MemoryReadOrigin origin = MemoryReadOrigin::Explicit);
	// After a guest write fault at vaddr (see MemoryTracker::ExpandWriteFault).
	// KYTY_FAULT_AHEAD_PAGES=<pages> sets the window (a power of two, default 16; 1: off).
	void                   ExpandWriteFault(uint64_t vaddr);
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false,
	                                  MemoryReadOrigin origin = MemoryReadOrigin::Explicit);
	// GPU command parser: starts/submits a real download and reports false until its
	// page publication has finished. Never treats stale-readable backing as ready.
	[[nodiscard]] bool     PollReadMemory(uint64_t vaddr, uint64_t size);
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	// FindBuffer, keeping `id` while it is a live buffer covering the range (as ObtainBuffer does):
	// buffers never overlap, so a live buffer over the range is the one FindBuffer would find.
	[[nodiscard]] BufferId RefindBuffer(BufferId id, uint64_t vaddr, uint64_t size) {
		return !IsBufferInvalid(id) && m_slot_buffers[id].IsInBounds(vaddr, size)
		           ? id
		           : FindBuffer(vaddr, size);
	}
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBuffer(uint64_t vaddr, uint64_t size,
	                                                        bool     is_written,
	                                                        bool     is_texel_buffer = false,
	                                                        BufferId id              = {});
	// Image/metadata publication already owns the texture-cache lock. Its caller
	// prepares any relevant aliases explicitly before acquiring this destination;
	// this path must not recursively prepare another buffer overwrite.
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImagePublication(uint64_t vaddr,
	                                                                          uint64_t size);
	[[nodiscard]] StreamBuffer&                GetUtilityBuffer(MemoryUsage usage) noexcept {
		switch (usage) {
			case MemoryUsage::Upload: return m_staging_buffer;
			case MemoryUsage::Stream: return ActiveStream();
			case MemoryUsage::Download: return m_download_buffer;
			case MemoryUsage::DeviceLocal: return m_device_buffer;
		}
		EXIT("BufferCache: invalid utility-buffer usage\n");
	}
	// The ring that per-draw stream copies go to (see bufferCache.cpp).
	[[nodiscard]] StreamBuffer& ActiveStream() noexcept;
	// Device-addressable ring for per-draw parameter records that shaders load by address.
	[[nodiscard]] StreamBuffer& GetDrawRecordBuffer() noexcept { return m_draw_record_buffer; }
	// The zero page's device address (written on first use): a table block's null buffers.
	[[nodiscard]] vk::DeviceAddress ZeroPageAddress() {
		EnsureBdaZeroPage();
		return m_bda_zero_page.BufferDeviceAddress();
	}
	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }
	[[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept { return &m_bda_pagetable_buffer; }
	// For dispatch snapshots: the cache buffer that holds a guest page (where BDA reads it) and
	// the page's offset in it, without creating one; a null buffer when the page has none.
	[[nodiscard]] std::pair<vk::Buffer, uint64_t> PeekPage(uint64_t vaddr);
	// KYTY_DEBUG_DEVICE_FAULT: once per frame an 8-byte readback of one page-table entry, compared
	// with the value written there (the combined build probes its zero-page entry). For tests,
	// KYTY_DEBUG_PAGETABLE_PROBE=<entry>:<hex value> sets it.
	void SetPageTableProbe(uint64_t entry, uint64_t expected);
	// KYTY_DEBUG_DEVICE_FAULT: prints which buffer (live, or freed within the last 4096 deletes)
	// holds a device address the driver reported faulting on.
	void DescribeDeviceAddress(uint64_t address) const;
	// Debugging aid: prints the cache buffer owning that guest page and its page-table entry.
	void DebugDescribePage(uint64_t vaddr);
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }
	[[nodiscard]] const Buffer* PeekFaultBuffer() const noexcept { return m_fault_manager.PeekFaultBuffer(); }
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	// Records a queue-ordered write and publishes its backing after native completion.
	// Unlike FillBuffer's CPU fast path, this also orders clean, unregistered labels.
	void WriteOrderedMemory(uint64_t vaddr, std::span<const uint8_t> bytes);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsPageGpuDirtyHint(uint64_t vaddr) const noexcept {
		return m_memory_tracker.IsPageGpuDirtyHint(vaddr);
	}
	// Changes whenever bytes become GPU-dirty or a download starts, which is also the only way a
	// GPU-dirty range can become clean again (see m_clean_pages). GPU thread only.
	[[nodiscard]] uint64_t GpuDirtyGeneration() const noexcept { return m_gpu_dirty_generation; }
	// Eager readback of hot pages: memory that CPU reads have faulted on. A write recorded to a
	// hot page is downloaded at the next flush point, so its bytes are usually published before
	// the CPU reads them again. OnCommandRecorded() marks writes of the bindings obtained so far
	// as recorded; only recorded writes are downloaded.
	void               OnCommandRecorded();
	void               RecordEagerReadbacks();
	void               ProcessFaultBuffer();
	void               ProcessExplodeLog() { m_fault_manager.ProcessExplodeLog(); }
	void               SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	void               PublishBdaHints(uint64_t vaddr, uint64_t size) noexcept;
	void               SynchronizeBdaLegacy(const RangeSet& mapped);
	[[nodiscard]] bool SynchronizeBdaSelective(const RangeSet& mapped);
	[[nodiscard]] bool CheckBdaHintInvariant(const RangeSet& mapped);
	// KYTY_DEBUG_BDA_STATS: accounts one PrepareBda synchronization (see bufferCache.cpp).
	[[nodiscard]] static bool BdaStatsEnabled();
	void                      NoteBdaPass(uint64_t elapsed_ns);
	void               TickFrame();
	void               RunGarbageCollector();

private:
	friend struct BufferCacheTestAccess;
	friend struct PerformanceMemoryTestAccess;

	bool IsBufferInvalid(BufferId id) const {
		const auto* buffer = m_slot_buffers.try_get(id);
		return buffer == nullptr || buffer->is_deleted;
	}

	using BufferMap = std::map<uint64_t, BufferId>;
	struct OverlapResult {
		BufferMap::iterator first;
		BufferMap::iterator last;
		uint64_t            begin;
		uint64_t            end;
		bool                has_stream_leap;
	};

	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS, 44, 20>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	void TouchBuffer(const Buffer& buffer);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferImpl(uint64_t vaddr, uint64_t size,
	                                                            bool is_written,
	                                                            bool is_texel_buffer, BufferId id,
	                                                            bool image_publication = false);
	// deferred: the pages' protections are queued (PageManager::DeferProtects).
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size, bool deferred = false);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Records downloads of the range's unarmed GPU-dirty pages, arms them, and queues their
	// publication, which finalizes them. Returns false when there was nothing to arm.
	// An immediate ordered store may supply its exact bytes for publication after the
	// same native completion, while other dirty bytes retain their actual GPU downloads.
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                        std::span<const uint8_t> ordered_bytes = {});
	// Thread_Gpu: arms the readback window around [vaddr, vaddr+size).
	void RecordReadback(uint64_t vaddr, uint64_t size, bool is_write, MemoryReadOrigin origin);
	// Other threads: publish GPU-dirty pages without draining Thread_Gpu.
	void ReadMemoryAsync(uint64_t vaddr, uint64_t size, bool is_write, MemoryReadOrigin origin);
	void MarkReadbackHot(uint64_t vaddr);
	// GPU writes that may still be in flight (KYTY_DEBUG_READBACK_AGE): NoteGpuWrite logs a
	// written range; Thread_Gpu, between commands, InflightWriteTick returns the latest tick of a
	// possibly unfinished write overlapping [begin, end) (0: none). Writes get their tick at the
	// next point between commands, so one is never dated earlier than its command buffer.
	// KYTY_READBACK_QUEUE=1 (ReadbackQueue). Thread_Gpu, for a guest thread's readback: when every
	// GPU write of the readback window has finished, arms its GPU-dirty pages for a download on
	// the readback queue and returns it; null otherwise. The guest thread then submits and
	// publishes it (PublishQueuedReadback); a priority operation at the arming tick does so too,
	// for waiters that only know that tick.
	struct QueuedReadback;
	[[nodiscard]] std::shared_ptr<QueuedReadback> QueueReadback(uint64_t vaddr, uint64_t size,
	                                                            bool is_write);
	void PublishQueuedReadback(QueuedReadback& request);
	void                   NoteGpuWrite(uint64_t vaddr, uint64_t size);
	[[nodiscard]] uint64_t InflightWriteTick(uint64_t begin, uint64_t end, uint64_t completed);
	[[nodiscard]] bool     GpuWriteLogComplete(uint64_t completed) const noexcept {
		return m_gpu_writes_on && completed >= m_gpu_writes_from;
	}
	void QueueEagerReadback(uint64_t vaddr, uint64_t size);
	// Whether a recorded download of these bytes has not finished publishing.
	[[nodiscard]] bool InFlightIntersects(uint64_t vaddr, uint64_t size);
	void               PruneInFlight();
	void               EnsureBdaZeroPage();
	[[nodiscard]] bool SynchronizeBdaWord(size_t word, const RangeSet& mapped);
	[[nodiscard]] bool SynchronizeBdaRegion(uint64_t region, const RangeSet& mapped);
	[[nodiscard]] bool SynchronizeDirtyOwners(const RegionBits& dirty, uint64_t region_begin,
	                                          uint64_t begin, uint64_t end);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	// Never registered, so never evicted, unmapped or overlapped by a game buffer.
	Buffer                                            m_bda_zero_page;
	bool                                              m_bda_zero_page_written = false;
	struct FreedBuffer {
		uint64_t bda  = 0;
		uint64_t size = 0;
		uint64_t cpu  = 0;
		uint64_t tick = 0;
	};
	std::array<FreedBuffer, 4096> m_freed_buffers {};
	size_t                       m_freed_next = 0;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	MemoryTracker                                     m_memory_tracker;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	StreamBuffer                                      m_draw_record_buffer;
	// KYTY_DEBUG_AB=streamhost: a stream ring in device memory for the A/B (see ActiveStream).
	std::unique_ptr<StreamBuffer> m_stream_device;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
	void     ProbePageTable();
	std::unique_ptr<Buffer>  m_probe_buffer;
	uint64_t                 m_probe_entry      = UINT64_MAX;
	uint64_t                 m_probe_expected   = 0;
	std::array<uint64_t, 4>  m_probe_ticks      {};
	uint64_t                 m_probe_frames     = 0;
	uint64_t                 m_probe_checked    = 0;
	uint64_t                 m_probe_mismatches = 0;
	uint64_t m_readback_token     = 0;
	// Tracker pages hit by CPU read faults, with the tick of their latest fault.
	static constexpr size_t HotReadbackPages = 64;
	std::unordered_map<uint64_t, uint64_t> m_hot_pages;
	std::vector<uint64_t>                  m_eager_pending; // Written by unrecorded commands.
	std::vector<uint64_t>                  m_eager_ready;   // Written by recorded commands.
	// Downloaded byte ranges whose backing is not written yet, oldest first. They left
	// m_gpu_modified_ranges when recorded; the other bytes of their pages are already valid.
	struct InFlightDownload {
		uint64_t                                   tick = 0;
		std::vector<std::pair<uint64_t, uint64_t>> ranges;
	};
	std::deque<InFlightDownload> m_inflight_downloads;
	struct GpuWrite {
		uint64_t begin = 0;
		uint64_t end   = 0;
		uint64_t tick  = 0;
	};
	std::unique_ptr<ReadbackQueue> m_readback_queue;
	std::vector<GpuWrite> m_gpu_writes;
	size_t                m_gpu_writes_head    = 0;
	size_t                m_gpu_writes_stamped = 0;
	bool                  m_gpu_writes_on      = false;
	// Writes before this tick were not logged (the log started or overflowed).
	uint64_t              m_gpu_writes_from    = UINT64_MAX;
	// HasGpuDirtyBytes: tracker pages found clean, valid while m_gpu_dirty_generation is
	// unchanged. It is bumped when bytes become GPU-dirty or a download starts; nothing else can
	// dirty a page. GPU thread only, like HasGpuDirtyBytes.
	struct CleanPage {
		uint64_t page       = UINT64_MAX;
		uint64_t generation = 0;
	};
	std::array<CleanPage, 64> m_clean_pages {};
	uint64_t                  m_gpu_dirty_generation = 1;
	// ObtainBuffer's stream-ring copies of the current BDA epoch, by range (see there).
	struct StreamCopy {
		uint64_t vaddr        = 0;
		uint64_t size         = 0;
		uint64_t epoch        = 0; // 0: none.
		uint64_t tick         = 0;
		uint64_t gpu_writes   = 0; // m_gpu_dirty_generation.
		uint64_t image_writes = 0; // TextureCache::GpuModifiedGeneration.
		Buffer*  stream       = nullptr;
		uint64_t offset       = 0;
	};
	static constexpr size_t StreamCopySlots = 4096;
	std::vector<StreamCopy> m_stream_copies = std::vector<StreamCopy>(StreamCopySlots);
	// KYTY_VERIFY_STREAM_REUSE=1: whether to copy again anyway (after comparing; see there).
	[[nodiscard]] static bool VerifyStreamReuse(const StreamCopy& copy);
	// ObtainBufferForImage's staged uploads, by range, with the time of the latest.
	struct ImageStage {
		uint64_t                              vaddr = 0;
		uint64_t                              size  = 0;
		std::chrono::steady_clock::time_point time {};
	};
	std::array<ImageStage, 256> m_image_stages {};
	[[nodiscard]] bool          IsRepeatedImageStage(uint64_t vaddr, uint64_t size);
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
