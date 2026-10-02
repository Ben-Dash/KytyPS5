#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/drainStats.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <string>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	if (Config::AsyncSubmitEnabled()) {
		// vkQueueSubmit was about 40% of Thread_Gpu in Astro Bot; a queue thread takes it over.
		m_command_scheduler.EnableAsyncSubmit();
	}
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	const bool gpu_thread = GuestGpu::IsGpuThread();
	struct SiteTimer {
		bool                                  enabled;
		bool                                  write;
		uint64_t                              address;
		std::chrono::steady_clock::time_point start;
		~SiteTimer() {
			if (enabled) {
				const auto elapsed = std::chrono::steady_clock::now() - start;
				DrainStats::RecordFaultSite(
				    DrainStats::t_fault_pc, address, write,
				    static_cast<uint64_t>(
				        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
			}
		}
	} site_timer {DrainStats::Enabled(), access == PageFaultAccess::Write, fault_vaddr,
	              DrainStats::Enabled() ? std::chrono::steady_clock::now()
	                                    : std::chrono::steady_clock::time_point {}};
	if (access == PageFaultAccess::Write) {
		DrainStats::ReasonScope reason(gpu_thread ? DrainStats::Reason::GpuThreadWriteFault
		                                          : DrainStats::Reason::GuestWriteFault);
		RecordUpload(UploadSource::Fault, fault_vaddr, 0x1000);
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
		if (gpu_thread) {
			// The command stream writes this (WRITE_DATA, fills): later draws must see it.
			AdvanceBdaEpoch(13);
		}
	} else {
		DrainStats::ReasonScope reason(gpu_thread ? DrainStats::Reason::GpuThreadReadFault
		                                          : DrainStats::Reason::GuestReadFault);
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	return true;
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	DrainStats::ReasonScope reason(DrainStats::Reason::KernelInvalidate);
	RecordUpload(UploadSource::Kernel, vaddr, size);
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	AdvanceBdaEpoch(14);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
	m_buffer_cache.PublishBdaHints(vaddr, size);
	AdvanceBdaEpoch(15);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		return;
	}
	{
		// Memory the GPU never mapped holds no cache entries: HandleFault and
		// InvalidateMemory gate on the same set. Skip the full GPU drain for it.
		std::shared_lock lock(m_mapped_ranges_mutex);
		if (!m_mapped_ranges.Intersects(vaddr, size)) {
			return;
		}
	}
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		DrainStats::ReasonScope reason(DrainStats::Reason::Unmap);
		if (m_command_scheduler.Active()) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
		AdvanceBdaEpoch(16);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

// KYTY_DEBUG_BDA_EPOCH=0 synchronizes before every draw that reads memory through addresses;
// KYTY_DEBUG_AB=bdaepoch alternates.
static bool BdaEpochEnabled() {
	static const bool enabled = [] {
		const char* text = std::getenv("KYTY_DEBUG_BDA_EPOCH");
		return text == nullptr || std::strcmp(text, "0") != 0;
	}();
	static const bool ab = AbSelected("bdaepoch");
	return enabled && !(ab && AbFeatureOff());
}

uint64_t RenderContext::CurrentBdaEpoch() const noexcept {
	return BdaEpochEnabled() ? m_bda_epoch.load(std::memory_order_acquire) : 0;
}

// KYTY_DEBUG_AB=cpwrite alternates.
void RenderContext::AdvanceBdaEpochForGpuWrite(uint8_t site, uint64_t vaddr,
                                               uint64_t size) noexcept {
	static const bool ab     = AbSelected("cpwrite");
	static const bool ranges = [] {
		const char* text = std::getenv("KYTY_BDA_GPU_WRITE_RANGES");
		return text == nullptr || std::strcmp(text, "0") != 0;
	}();
	if (ab && AbFeatureOff()) {
		return;
	}
	if (!ranges || vaddr == 0 || size == 0 || size > 0x1000) {
		AdvanceBdaEpoch(site);
		return;
	}
	{
		std::scoped_lock lock(m_bda_ranges_mutex);
		if (m_bda_ranges.size() >= 256) {
			// Too many to keep: synchronize everything instead.
			m_bda_ranges.clear();
			m_bda_ranges_pending.store(false, std::memory_order_relaxed);
			AdvanceBdaEpoch(site);
			return;
		}
		m_bda_ranges.emplace_back(vaddr, size);
		m_bda_ranges_pending.store(true, std::memory_order_release);
	}
	m_bda_epoch.fetch_add(1, std::memory_order_release);
}

// Draws reading memory through addresses must see the CPU writes made before their submission,
// so the first such draw of each epoch uploads every CPU-dirty page. Meanwhile guest threads keep
// writing later frames' data (Astro Bot's Sky Garden faults about 150k pages/s): uploading and
// re-protecting those pages before every such draw only brings the next fault sooner, since no
// draw of this submission may read them. Everything that can make newer CPU writes visible to
// later draws starts an epoch: a submission starting or resuming, packets that read guest memory
// (waits, conditions, predication), new page-table entries (buffer registration), GPU mappings,
// kernel invalidations and the GPU thread's own writes.
void RenderContext::PrepareBda() {
	static const bool sites = std::getenv("KYTY_DEBUG_BDA_SITES") != nullptr;
	RecordUpload(UploadSource::BdaPass, 0, 0);
	m_fault_process_pending = true;
	m_bda_passes++;
	const auto epoch        = m_bda_sync_epoch.load(std::memory_order_acquire);
	if (epoch == m_bda_synced_epoch && BdaEpochEnabled()) {
		if (m_bda_ranges_pending.load(std::memory_order_acquire)) [[unlikely]] {
			// Only the GPU's own small writes since the last synchronization.
			std::vector<std::pair<uint64_t, uint64_t>> ranges;
			{
				std::scoped_lock lock(m_bda_ranges_mutex);
				ranges.swap(m_bda_ranges);
				m_bda_ranges_pending.store(false, std::memory_order_relaxed);
			}
			std::shared_lock lock(m_mapped_ranges_mutex);
			for (const auto& [vaddr, size]: ranges) {
				m_buffer_cache.SynchronizeBuffersInRange(vaddr, size);
			}
		}
		return;
	}
	// An epoch started during the synchronization below leaves the next call synchronizing.
	m_bda_synced_epoch = epoch;
	// The whole synchronization covers the GPU's small writes too.
	if (m_bda_ranges_pending.load(std::memory_order_acquire)) {
		std::scoped_lock lock(m_bda_ranges_mutex);
		m_bda_ranges.clear();
		m_bda_ranges_pending.store(false, std::memory_order_relaxed);
	}
	const auto begin   = sites ? std::chrono::steady_clock::now()
	                           : std::chrono::steady_clock::time_point {};
	if (sites) [[unlikely]] {
		m_bda_site_syncs[std::min<size_t>(m_bda_last_site.load(std::memory_order_relaxed),
		                                  m_bda_site_syncs.size() - 1u)]++;
	}
	RecordUpload(UploadSource::BdaSync, 0, 0);
	{
		std::shared_lock lock(m_mapped_ranges_mutex);
		const auto       mode = Config::GetBdaSyncMode();
		if (mode == Config::BdaSyncMode::Legacy ||
		    !m_buffer_cache.SynchronizeBdaSelective(m_mapped_ranges)) {
			m_buffer_cache.SynchronizeBdaLegacy(m_mapped_ranges);
		}
		if (mode == Config::BdaSyncMode::SelectiveChecked) {
			EXIT_IF(!m_buffer_cache.CheckBdaHintInvariant(m_mapped_ranges));
		}
	}
	if (sites) [[unlikely]] {
		const auto now = std::chrono::steady_clock::now();
		m_bda_sync_ns += static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(now - begin).count());
		const auto now_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
		if (m_bda_report_time == 0) {
			m_bda_report_time = now_ms;
		} else if (now_ms - m_bda_report_time >= 5000) {
			// Sites: 1 rewind, 2 cond-exec, 3 branch, 4 wait-reg-mem, 5 write-data, 6 cp write,
			// 7 submission, 8 predication, 9 buffer register, 10 read fault, 11/12 fill,
			// 13 GPU-thread write fault, 14 kernel invalidate, 15 map, 16 unmap, 0 other.
			std::string line = fmt::format("bda-sites: passes/s={:.0f} sync-ms/s={:.1f} |",
			                               static_cast<double>(m_bda_passes) * 1000.0 /
			                                   static_cast<double>(now_ms - m_bda_report_time),
			                               static_cast<double>(m_bda_sync_ns) / 1e6 * 1000.0 /
			                                   static_cast<double>(now_ms - m_bda_report_time));
			for (size_t site = 0; site < m_bda_site_syncs.size(); site++) {
				if (m_bda_site_syncs[site] != 0) {
					line += fmt::format(" {}:{}", site, m_bda_site_syncs[site]);
				}
			}
			std::printf("%s\n", line.c_str());
			m_bda_site_syncs.fill(0);
			m_bda_passes      = 0;
			m_bda_sync_ns     = 0;
			m_bda_report_time = now_ms;
		}
	}
}

// KYTY_DEBUG_MIP_STATS=1 prints, every 300 reports (every 30 during a relief), whether draws
// marked a counter since the last report and how many counters report no samples.
static bool MipStatsDebugEnabled() {
	static const bool enabled = std::getenv("KYTY_DEBUG_MIP_STATS") != nullptr;
	return enabled;
}

// The last NoteStreamingThrash, in steady_clock ticks; 0: none yet.
static std::atomic<int64_t> g_streaming_thrash_time {0};

void NoteStreamingThrash() noexcept {
	g_streaming_thrash_time.store(std::chrono::steady_clock::now().time_since_epoch().count(),
	                              std::memory_order_relaxed);
}

void RenderContext::ReportMipStats(void* dst, uint32_t size, bool reset) {
	// A report is a 64-byte header, whose first dword marks it complete, then a 64-bit value per
	// counter, as Astro Bot's texture streamer reads them: the finest mip sampled in bits 56-59
	// (15: none) and a sample count in bits 0-23. The streamer moves a texture's detail to the mip
	// reported, and lets textures that report no samples drop to coarser mips.
	//
	// Every counter reports mip 0: draws only tell which textures they bind, not the mips they
	// sample, and reporting no samples for counters that no draw marked kept Astro Bot reloading
	// textures throughout play (a texture at full detail no longer carries its counter, so the
	// counter looks unused). But when every texture claims full detail, the streamer can
	// overflow its memory and reload the same textures in turn forever (Astro Bot at times never
	// left the warp to a level). So when the game is seen thrashing (NoteStreamingThrash), for
	// ReliefTime counters that no draw marked in the last InUseReports reports report no
	// samples, and their textures drop out of the way. The reloads that this causes in turn are
	// no new thrash: the next relief waits for a thrash noted a Cooldown after this one.
	constexpr uint32_t HeaderSize   = 64;
	constexpr uint64_t SampledMip0  = 0;
	constexpr uint64_t NotSampled   = uint64_t {0xf} << 56u;
	constexpr uint64_t InUseReports = 300;
	constexpr std::chrono::steady_clock::duration ReliefTime = std::chrono::seconds(15);
	constexpr std::chrono::steady_clock::duration Cooldown   = std::chrono::seconds(20);

	m_mip_stats_reports++;
	bool marked_any = false;
	for (uint32_t id = 0; id < m_mip_stats_last_marked.size(); id++) {
		if (((m_mip_stats_counters[id / 64u] >> (id % 64u)) & 1u) != 0) {
			m_mip_stats_last_marked[id] = m_mip_stats_reports;
			marked_any                  = true;
		}
	}
	if (marked_any) {
		m_mip_stats_last_marked_any = m_mip_stats_reports;
	}
	if (reset) {
		m_mip_stats_counters = {};
	}
	const auto in_use = [&](uint64_t last_marked) {
		return last_marked != 0 && m_mip_stats_reports - last_marked <= InUseReports;
	};
	const auto now         = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto thrash_time = g_streaming_thrash_time.load(std::memory_order_relaxed);
	if (m_mip_stats_relief_end == 0 && thrash_time != 0 && thrash_time > m_mip_stats_rearm_time) {
		m_mip_stats_relief_end = now + ReliefTime.count();
	}
	if (m_mip_stats_relief_end != 0 && now >= m_mip_stats_relief_end) {
		m_mip_stats_relief_end = 0;
		m_mip_stats_rearm_time = now + Cooldown.count();
	}
	const bool relief = m_mip_stats_relief_end != 0;
	// Until draws mark some counter, nothing is known to be unused.
	const bool counting = relief && in_use(m_mip_stats_last_marked_any);

	std::memset(dst, 0, size);
	if (size < sizeof(uint32_t)) {
		return;
	}
	*static_cast<uint32_t*>(dst) = 1;
	const auto counters =
	    std::min<uint32_t>(size > HeaderSize ? (size - HeaderSize) / 8u : 0u,
	                       static_cast<uint32_t>(m_mip_stats_last_marked.size()));
	auto*    values = reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(dst) + HeaderSize);
	uint32_t unused = 0;
	for (uint32_t id = 0; id < counters; id++) {
		const bool sampled = !counting || in_use(m_mip_stats_last_marked[id]);
		values[id]         = sampled ? SampledMip0 : NotSampled;
		unused += sampled ? 0u : 1u;
	}
	const bool print = m_mip_stats_reports % 300 == 0 || (counting && m_mip_stats_reports % 30 == 0);
	if (MipStatsDebugEnabled() && print) {
		std::printf("mip-stats: report %llu marked=%d relief=%d unused=%u of %u\n",
		            static_cast<unsigned long long>(m_mip_stats_reports), marked_any ? 1 : 0,
		            relief ? 1 : 0, unused, counters);
		std::fflush(stdout);
	}
}

void RenderContext::TickFrame() {
	m_texture_cache.TickFrame();
	m_buffer_cache.TickFrame();
}

void RenderContext::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
