#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_

#include "common/assert.h"
#include "common/switches.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <xmmintrin.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#undef MemoryBarrier
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {

class TrackingSpinLock final {
public:
	void lock() noexcept {
		const auto thread = CurrentThread();
		while (m_lock.test_and_set(std::memory_order_acquire)) {
			EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) == thread);
#if defined(__x86_64__) || defined(_M_X64)
			_mm_pause();
#elif defined(_M_ARM64)
			__yield();
#elif defined(__aarch64__)
			asm volatile("yield");
#endif
		}
		m_owner.store(thread, std::memory_order_relaxed);
	}
	void unlock() noexcept {
		EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) != CurrentThread());
		m_owner.store(0, std::memory_order_relaxed);
		m_lock.clear(std::memory_order_release);
	}

private:
	static uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return GetCurrentThreadId();
#elif defined(__APPLE__)
		// mach thread port is a nonzero per-thread id (0 is the "no owner" sentinel).
		static thread_local const uint32_t tid =
		    static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
		return tid;
#elif defined(__linux__)
		static thread_local const uint32_t tid = static_cast<uint32_t>(::syscall(SYS_gettid));
		return tid;
#else
		EXIT("region tracking thread identity is unsupported on this platform\n");
#endif
	}

	std::atomic_flag     m_lock = ATOMIC_FLAG_INIT;
	std::atomic_uint32_t m_owner {0};
};

static_assert(std::atomic_uint32_t::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

// Sets region hint bits, then the hint word's summary bit. Consumers exchange a summary word
// before its hint words, so a publisher that sees its summary bit already set (sequentially
// consistent with that exchange) knows the consumer will still claim these hint bits.
inline void PublishBdaHintBits(std::atomic<uint64_t>& hint_word, uint64_t bits,
                               std::atomic<uint64_t>& summary_word,
                               uint64_t summary_bit) noexcept {
	hint_word.fetch_or(bits, std::memory_order_seq_cst);
	// Skip the shared read-modify-write when possible: one summary word covers 16 GiB.
	if ((summary_word.load(std::memory_order_seq_cst) & summary_bit) == 0) {
		summary_word.fetch_or(summary_bit, std::memory_order_seq_cst);
	}
}

// Readback state of the queried pages: GPU-dirty pages are either armed (a recorded download
// publishes them at `tick`) or unarmed (their bytes still need a download).
struct ReadbackState {
	bool     gpu_dirty = false;
	bool     unarmed   = false;
	uint64_t tick      = 0; // Highest publication tick among armed pages.
};

// PrepareBda's synchronizing passes so far (see RegionManager::HotWrites).
inline std::atomic<uint32_t> g_bda_pass_serial {0};

#if defined(KYTY_BINDLESS_LIFECYCLE_TEST_ACCESS)
// Harness-only work observer; absent from emulator builds.
inline thread_local uint64_t* g_region_protection_mask_evaluations = nullptr;
#endif

class RegionManager final {
public:
	RegionManager(PageManager& page_manager, uint64_t cpu_addr,
	              std::atomic<uint64_t>& bda_hint_word, std::atomic<uint64_t>& bda_summary_word,
	              std::atomic<int64_t>& armed_pages)
	    : m_page_manager(page_manager), m_cpu_addr(cpu_addr), m_bda_hint_word(bda_hint_word),
	      m_bda_hint_mask(uint64_t {1} << ((cpu_addr / TRACKER_REGION_SIZE) % 64u)),
	      m_bda_summary_word(bda_summary_word),
	      m_bda_summary_mask(uint64_t {1} << ((cpu_addr / TRACKER_REGION_SIZE / 64u) % 64u)),
	      m_armed_pages(armed_pages) {
		if (m_cpu_addr % TRACKER_REGION_SIZE != 0) {
			EXIT("invalid region tracking manager construction\n");
		}
		m_cpu_dirty.Fill();
		m_writable.Fill();
		m_readable.Fill();
	}

	KYTY_CLASS_NO_COPY(RegionManager);

	// The region split into 64 slices of this many pages (64 KiB); see CpuDirtySlices.
	static constexpr size_t CPU_DIRTY_SLICE_PAGES = TRACKER_REGION_PAGES / 64;
	static_assert(TRACKER_REGION_PAGES % 64 == 0 && CPU_DIRTY_SLICE_PAGES != 0);

	// Lock-free: bit s is set while a page of slice s may be CPU-dirty. Every change to the
	// CPU-dirty bits publishes this under the lock before the pages' write protection changes, so
	// a clear bit means that nothing written there by the CPU still needs uploading. A reader
	// that sees a clear bit just before a write is published is ordered as if it had taken the
	// lock before that write.
	[[nodiscard]] uint64_t CpuDirtySlices() const noexcept {
		return m_cpu_dirty_slices.load(std::memory_order_acquire);
	}

	[[nodiscard]] uint64_t GetCpuAddr() const { return m_cpu_addr; }
	void                   PublishBdaHint() const noexcept {
		PublishBdaHintBits(m_bda_hint_word, m_bda_hint_mask, m_bda_summary_word,
		                   m_bda_summary_mask);
	}
	// Caller holds lock. This snapshot is only a discovery hint; uploads read the live bits.
	[[nodiscard]] const RegionBits& CpuDirtyBits() const noexcept { return m_cpu_dirty; }
	template <DirtySource source>
	[[nodiscard]] bool IsModified(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(m_cpu_addr + offset, size);
		return GetBits<source>().AnyInRange(start, end);
	}

	template <DirtySource source, bool enable>
	void ChangeState(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		if constexpr (source == DirtySource::Cpu && enable) {
			if (m_gpu_dirty.AnyInRange(start, end)) {
				EXIT("CPU dirty state conflicts with GPU dirty state\n");
			}
		}
		if constexpr (source == DirtySource::Gpu && enable) {
			if (m_cpu_dirty.AnyInRange(start, end)) {
				EXIT("GPU dirty state conflicts with CPU dirty state\n");
			}
		}
		if constexpr (source == DirtySource::Gpu && enable) {
			// The validated, locked range already has exactly the required read protection.
			// Pending publication or stale-readable pages still require the transition below.
			if (m_gpu_dirty.AllInRange(start, end) &&
			    (m_arms == nullptr || !m_arms->armed.AnyInRange(start, end)) &&
			    !m_stale_readable.AnyInRange(start, end)) {
				return;
			}
		}
		auto& bits = GetBits<source>();
		if constexpr (source == DirtySource::Gpu) {
			// A new GPU write or an explicit unmark supersedes any recorded download: its
			// publication must no longer clear these pages.
			Disarm(start, end);
			m_stale_readable.UnsetRange(start, end);
		}
		if constexpr (enable) {
			bits.SetRange(start, end);
		} else {
			bits.UnsetRange(start, end);
		}
		if constexpr (source == DirtySource::Cpu) {
			PublishCpuDirtySlices();
		}
		if constexpr (source == DirtySource::Cpu && enable) {
			// Publish every write, even if already dirty: a selective pass may have consumed
			// the previous hint. The caller still holds the region lock while publishing.
			PublishBdaHint();
		}
		if constexpr (source == DirtySource::Cpu) {
			UpdateProtection<!enable, false>();
		} else {
			UpdateProtection<enable, true>();
		}
	}

	// Caller holds lock: the range's pages that are not GPU-dirty become CPU-dirty.
	void MarkCpuDirtyUnlessGpuDirty(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		RegionBits add;
		add.SetRange(start, end);
		add &= ~m_gpu_dirty;
		add &= ~m_cpu_dirty;
		if (add.None()) {
			return;
		}
		m_cpu_dirty |= add;
		PublishCpuDirtySlices();
		PublishBdaHint();
		UpdateProtection<false, false>();
	}

	// keep_epoch (CPU, clear): the BDA epoch of a read-only upload, or 0. Then hot written pages
	// (see HotWrites) stay CPU-dirty and writable instead of being protected again, and
	// are reported once per epoch. With 0 the range's pages lose that status.
	template <DirtySource source, bool clear, typename Func>
	void ForEachModifiedRange(uint64_t vaddr, uint64_t size, Func&& func, uint64_t keep_epoch = 0) {
		const auto [start, end] = GetPageRange(vaddr, size);
		auto&      bits         = GetBits<source>();
		if constexpr (source == DirtySource::Cpu) {
			// Every CPU-dirty change updates the write protection, so with no dirty page in the
			// range, clearing it and updating the protection would change nothing. Buffers are
			// bound per draw, usually clean.
			if (!bits.AnyInRange(start, end)) {
				return;
			}
		}
		RegionBits mask(bits, start, end);
		if constexpr (clear) {
			if constexpr (source == DirtySource::Gpu) {
				Disarm(start, end);
				m_stale_readable.UnsetRange(start, end);
			}
			RegionBits kept;
			if constexpr (source == DirtySource::Cpu) {
				if (keep_epoch == 0) {
					if (m_hot_writes != nullptr) {
						m_hot_writes->unwatched.UnsetRange(start, end);
					}
				} else if (HotWritesEnabled()) {
					if (m_hot_writes == nullptr) {
						m_hot_writes = std::make_unique<HotWrites>();
					}
					auto& hot = *m_hot_writes;
					if (hot.synced_epoch != keep_epoch) {
						hot.synced.Clear();
						hot.synced_epoch = keep_epoch;
						if (++hot.epochs >= HotWrites::DemoteEpochs) {
							// Try protecting them again: pages no longer written every pass
							// return to normal tracking, the others qualify again.
							hot.unwatched.Clear();
							hot.streak.fill(0);
							hot.epochs = 0;
						}
					}
					kept = mask & hot.unwatched;
					// Reported already this epoch: nothing a draw of it must see is new.
					mask &= ~(kept & hot.synced);
					hot.synced |= kept;
					CountDirtyPasses(hot, mask & ~kept);
				}
			}
			bits.UnsetRange(start, end);
			if constexpr (source == DirtySource::Cpu) {
				if (!kept.None()) {
					bits |= kept;
					// The next epoch's pass must visit them again.
					PublishBdaHint();
				}
				PublishCpuDirtySlices();
				UpdateProtection<true, false>();
			} else {
				UpdateProtection<false, true>();
			}
		}
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}

	// Calls func(address, bytes) for runs of GPU-dirty pages that no recorded download covers.
	// Caller holds lock.
	template <typename Func>
	void ForEachUnarmedGpuRange(uint64_t vaddr, uint64_t size, Func&& func) {
		const auto [start, end] = GetPageRange(vaddr, size);
		RegionBits mask(m_gpu_dirty, start, end);
		if (m_arms != nullptr) {
			for (const auto [first, last]: RegionBits(m_arms->armed, start, end)) {
				mask.UnsetRange(first, last);
			}
		}
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}

	// Arms the GPU-dirty, unarmed pages of the range: a download recorded with `token` publishes
	// them at `tick`. Caller holds lock.
	void Arm(uint64_t vaddr, uint64_t size, uint64_t token, uint64_t tick) {
		const auto [start, end] = GetPageRange(vaddr, size);
		if (m_arms == nullptr) {
			m_arms = std::make_unique<Arms>();
		}
		int64_t armed = 0;
		for (size_t page = start; page < end; page++) {
			if (m_gpu_dirty.Get(page) && !m_arms->armed.Get(page)) {
				m_arms->armed.Set(page);
				m_arms->token[page] = token;
				m_arms->tick[page]  = tick;
				armed++;
			}
		}
		m_armed_pages.fetch_add(armed, std::memory_order_relaxed);
	}

	// Publication of `token` finished: pages still armed by it are no longer GPU-dirty. Pages
	// re-dirtied or re-armed since keep their state. Caller holds lock.
	void Finalize(uint64_t vaddr, uint64_t size, uint64_t token) {
		if (m_arms == nullptr) {
			return;
		}
		const auto [start, end] = GetPageRange(vaddr, size);
		int64_t    finalized    = 0;
		for (size_t page = start; page < end; page++) {
			if (m_arms->armed.Get(page) && m_arms->token[page] == token) {
				m_arms->armed.Unset(page);
				m_gpu_dirty.Unset(page);
				m_stale_readable.Unset(page);
				finalized++;
			}
		}
		if (finalized != 0) {
			m_armed_pages.fetch_sub(finalized, std::memory_order_relaxed);
			UpdateProtection<false, true>();
		}
	}

	// Caller holds lock.
	void SetReadbackHot(uint64_t vaddr, uint64_t size, bool hot) {
		const auto [start, end] = GetPageRange(vaddr, size);
		if (hot) {
			m_hot.SetRange(start, end);
		} else {
			m_hot.UnsetRange(start, end);
		}
	}

	// Relaxed readback: lets guest threads read hot GPU-dirty pages of the range that a recorded
	// download is already publishing, with their previous bytes, until that publication or
	// the next GPU write to them. Returns false, granting nothing, when a GPU-dirty page of
	// the range is not armed or not hot. Only hot pages, which guest reads keep faulting on,
	// are opened: Thread_Gpu may read other GPU-dirty pages directly and rely on the fault.
	// Caller holds lock.
	[[nodiscard]] bool GrantStaleRead(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		bool       granted      = false;
		for (const auto [first, last]: RegionBits(m_gpu_dirty, start, end)) {
			for (size_t page = first; page < last; page++) {
				if (m_arms == nullptr || !m_arms->armed.Get(page) || !m_hot.Get(page)) {
					return false;
				}
			}
			granted = true;
		}
		if (!granted) {
			return false;
		}
		for (const auto [first, last]: RegionBits(m_gpu_dirty, start, end)) {
			for (size_t page = first; page < last; page++) {
				m_stale_readable.Set(page);
			}
		}
		UpdateProtection<false, true>();
		return true;
	}

	// Caller holds lock.
	void QueryReadback(uint64_t vaddr, uint64_t size, ReadbackState& state) const {
		const auto [start, end] = GetPageRange(vaddr, size);
		for (const auto [first, last]: RegionBits(m_gpu_dirty, start, end)) {
			state.gpu_dirty = true;
			for (size_t page = first; page < last; page++) {
				if (m_arms != nullptr && m_arms->armed.Get(page)) {
					state.tick = std::max(state.tick, m_arms->tick[page]);
				} else {
					state.unarmed = true;
				}
			}
		}
	}

	// Lock-free and possibly stale: whether the page holding vaddr is GPU-dirty.
	[[nodiscard]] bool GpuDirtyHint(uint64_t vaddr) const noexcept {
		return m_gpu_dirty.GetRelaxed((vaddr - m_cpu_addr) / TRACKER_PAGE_SIZE);
	}

	// Caller holds lock.
	[[nodiscard]] bool HasArmed(uint64_t vaddr, uint64_t size) const {
		if (m_arms == nullptr) {
			return false;
		}
		const auto [start, end] = GetPageRange(vaddr, size);
		return m_arms->armed.AnyInRange(start, end);
	}

	TrackingSpinLock lock;

private:
	struct Arms {
		RegionBits                                 armed;
		std::array<uint64_t, TRACKER_REGION_PAGES> token {};
		std::array<uint64_t, TRACKER_REGION_PAGES> tick {};
	};

	// Hot written pages. Guest threads rewrite some pages (per-frame constants, streaming data)
	// right after each upload protected them again: ASTRO BOT's Sky Garden took ~350k write
	// faults a second (about 8 us each on the guest threads) and the GPU thread ~9k protection
	// changes (~80 ms a second). A page that an upload found dirty in StreakLimit consecutive BDA
	// passes stays CPU-dirty and writable; uploads copy it once per BDA epoch (see
	// ForEachModifiedRange), as they did anyway. KYTY_HOT_WRITES=0 protects every page as before.
	struct HotWrites {
		static constexpr uint8_t  StreakLimit  = 4;
		static constexpr uint32_t DemoteEpochs = 1024; // A few seconds of epochs.
		std::array<uint32_t, TRACKER_REGION_PAGES> last_pass {};
		std::array<uint8_t, TRACKER_REGION_PAGES>  streak {};
		RegionBits                                 unwatched;
		RegionBits                                 synced; // Reported in synced_epoch.
		uint64_t                                   synced_epoch = 0;
		uint32_t                                   epochs       = 0;
	};

	static bool HotWritesEnabled() {
		static const bool enabled = [] {
			const char* text = KYTY_GETENV("KYTY_HOT_WRITES");
			return text == nullptr || text[0] != '0';
		}();
		return enabled;
	}

	// Caller holds lock: an upload is about to clear these dirty pages.
	static void CountDirtyPasses(HotWrites& hot, const RegionBits& cleared) {
		const auto pass = g_bda_pass_serial.load(std::memory_order_relaxed);
		for (const auto [first, last]: cleared) {
			for (size_t page = first; page < last; page++) {
				const auto previous = hot.last_pass[page];
				if (previous == pass) {
					continue;
				}
				auto& streak = hot.streak[page];
				streak       = previous + 1 != pass                ? 1
				               : streak < HotWrites::StreakLimit ? static_cast<uint8_t>(streak + 1)
				                                                 : streak;
				if (streak >= HotWrites::StreakLimit) {
					hot.unwatched.Set(page);
				}
				hot.last_pass[page] = pass;
			}
		}
	}

	void Disarm(size_t start, size_t end) {
		if (m_arms == nullptr) {
			return;
		}
		int64_t disarmed = 0;
		for (const auto [first, last]: RegionBits(m_arms->armed, start, end)) {
			disarmed += static_cast<int64_t>(last - first);
		}
		if (disarmed != 0) {
			m_arms->armed.UnsetRange(start, end);
			m_armed_pages.fetch_sub(disarmed, std::memory_order_relaxed);
		}
	}

	// Caller holds lock, after changing the CPU-dirty bits and before changing the protection.
	void PublishCpuDirtySlices() noexcept {
		m_cpu_dirty_slices.store(m_cpu_dirty.SliceSummary(), std::memory_order_release);
	}

	template <bool track, bool is_read>
	void UpdateProtection() {
#if defined(KYTY_BINDLESS_LIFECYCLE_TEST_ACCESS)
		if (g_region_protection_mask_evaluations != nullptr) {
			++*g_region_protection_mask_evaluations;
		}
#endif
		const auto protection = is_read ? ~m_gpu_dirty | m_stale_readable : m_cpu_dirty;
		auto&      previous   = is_read ? m_readable : m_writable;
		auto       mask       = protection ^ previous;
		if (mask.None()) {
			return;
		}
		previous = protection;
		m_page_manager.UpdatePageWatchersForRegion<track, is_read>(m_cpu_addr, mask);
	}

	template <DirtySource source>
	RegionBits& GetBits() {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	template <DirtySource source>
	const RegionBits& GetBits() const {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	[[nodiscard]] std::pair<size_t, size_t> GetPageRange(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || vaddr < m_cpu_addr || vaddr >= m_cpu_addr + TRACKER_REGION_SIZE ||
		    size > m_cpu_addr + TRACKER_REGION_SIZE - vaddr) {
			EXIT("range lies outside its tracking region\n");
		}
		const auto offset = vaddr - m_cpu_addr;
		return {static_cast<size_t>(offset / TRACKER_PAGE_SIZE),
		        static_cast<size_t>((offset + size + TRACKER_PAGE_SIZE - 1) / TRACKER_PAGE_SIZE)};
	}

	PageManager& m_page_manager;
	uint64_t     m_cpu_addr = 0;
	RegionBits   m_cpu_dirty;
	// See CpuDirtySlices. A new region starts fully CPU-dirty.
	std::atomic<uint64_t> m_cpu_dirty_slices {~uint64_t {0}};
	RegionBits   m_gpu_dirty;
	RegionBits   m_writable;
	RegionBits   m_readable;
	// GPU-dirty pages guest threads may read with their previous bytes (GrantStaleRead).
	RegionBits   m_stale_readable;
	// Pages guest reads keep faulting on (BufferCache's hot readback pages).
	RegionBits   m_hot;
	std::atomic<uint64_t>& m_bda_hint_word;
	uint64_t               m_bda_hint_mask;
	std::atomic<uint64_t>& m_bda_summary_word;
	uint64_t               m_bda_summary_mask;
	// Allocated when a page is first armed; most regions never hold a pending readback.
	std::unique_ptr<Arms>  m_arms;
	// Allocated at the first counted write fault.
	std::unique_ptr<HotWrites> m_hot_writes;
	std::atomic<int64_t>&  m_armed_pages;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
