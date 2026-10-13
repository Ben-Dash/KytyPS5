#ifndef KYTY_WAIT_POLL_CAUSAL_DIAGNOSTIC_H_
#define KYTY_WAIT_POLL_CAUSAL_DIAGNOSTIC_H_
// Temporary diagnostic only. No ownership queries, guest reads, waits or notifications.
#include "graphics/host_gpu/renderer/labelCausalTrace.h"
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace Libs::Graphics::WaitPollDiagnostic {
inline constexpr uint32_t QueueCount = 57, SlotCount = QueueCount * 2;
inline constexpr uint64_t RowLimit = 16384;
enum class PollPath : uint32_t {
  NotPolled, FallbackReady, TrackerClean, DownloadNowClean, UnpublishedSubmitted, UnpublishedFlushed,
  Count
};
struct Input {
  uint64_t owner=0, submit=0, packet=0, address=0, ref=0, mask=0;
  uint32_t queue=0, stream=0, width=0, func=0, poll=0, operation=0;
};
struct Totals {
  uint64_t attempts=0, unpublished=0, comparator_false=0, comparator_true=0;
  uint64_t poll_ns=0, read_ns=0, max_poll_ns=0, max_read_ns=0;
  std::array<uint64_t,static_cast<size_t>(PollPath::Count)> paths{};
};
struct Episode {
  Input input{};
  uint64_t id=0, first_ns=0, calls=0, unpublished=0, comparator_false=0;
  uint64_t poll_ns=0, read_ns=0, last_value=0;
  PollPath last_path=PollPath::Count;
  bool had_value=false;
};
inline std::array<Totals,SlotCount> totals{};
inline std::array<Episode,SlotCount> episodes{};
inline std::mutex mutex;
inline uint64_t next_episode=0, requested_rows=0, recorded_rows=0, suppressed_rows=0;
inline uint64_t bad_slots=0, superseded_episodes=0, cv_calls=0, cv_timeouts=0, cv_signalled=0;
inline uint64_t cv_ns=0, cv_max_ns=0, source_notifies=0;
inline std::array<uint64_t,6> cv_histogram{};
inline bool dumped=false;
inline bool Enabled() noexcept {
  static const bool enabled=[] {
    const char* p=KYTY_GETENV("KYTY_WAIT_POLL_DIAGNOSTIC");
    return p!=nullptr && std::strcmp(p,"1")==0;
  }();
  return enabled;
}
inline bool Active() noexcept {
  return Enabled() && LabelCausalTrace::active.load(std::memory_order_acquire);
}
// Call under this diagnostic's mutex. The label ring never calls back into it.
inline void ForwardRow(LabelCausalTrace::Stage stage, uint64_t owner=0, uint64_t tick=0,
    uint64_t token=0, uint64_t address=0, uint64_t size=0, uint64_t value=0,
    uint64_t pc=0, uint64_t submit=0) noexcept {
  ++requested_rows;
  if(recorded_rows==RowLimit) { ++suppressed_rows; return; }
  ++recorded_rows;
  LabelCausalTrace::Record(stage,owner,tick,token,address,size,value,pc,submit);
}
inline bool Same(const Input& a,const Input& b) noexcept {
  return a.owner==b.owner && a.submit==b.submit && a.packet==b.packet &&
    a.address==b.address && a.width==b.width && a.func==b.func && a.ref==b.ref &&
    a.mask==b.mask && a.poll==b.poll && a.operation==b.operation;
}
class Attempt;
inline thread_local Attempt* current=nullptr;
class Attempt {
  Input input{};
  uint64_t begin=0, read_begin=0;
  Attempt* previous=nullptr;
  bool on=false;
public:
  PollPath path=PollPath::NotPolled;
  uint64_t armed_tick=0;
  explicit Attempt(const Input& value,bool parser_context):input(value) {
    on=parser_context && Active();
    if(!on) return;
    begin=LabelCausalTrace::Now();
    previous=current; current=this;
  }
  ~Attempt() { if(on) current=previous; }
  void ReadBegin() noexcept { if(on) read_begin=LabelCausalTrace::Now(); }
  void Finish(bool unpublished,bool satisfied,uint64_t actual=0) noexcept {
    if(!on) return;
    const auto end=LabelCausalTrace::Now();
    const auto poll_elapsed=(read_begin!=0 ? read_begin : end)-begin;
    const auto read_elapsed=read_begin!=0 ? end-read_begin : 0;
    std::lock_guard lock(mutex);
    if(dumped || !Active()) return;
    if(input.queue>=QueueCount || input.stream>1) { ++bad_slots; return; }
    const auto slot=input.queue*2+input.stream;
    auto& count=totals[slot];
    ++count.attempts; count.unpublished+=unpublished;
    count.comparator_false+=!unpublished&&!satisfied;
    count.comparator_true+=!unpublished&&satisfied;
    count.poll_ns+=poll_elapsed; count.read_ns+=read_elapsed;
    count.max_poll_ns=std::max(count.max_poll_ns,poll_elapsed);
    count.max_read_ns=std::max(count.max_read_ns,read_elapsed);
    ++count.paths[static_cast<size_t>(path)];
    auto& episode=episodes[slot];
    if(episode.id!=0 && !Same(episode.input,input)) {
      ++superseded_episodes; episode={};
    }
    if(episode.id==0 && satisfied && !unpublished) return; // No ring row for immediate successes.
    if(episode.id==0) {
      episode.input=input; episode.id=++next_episode; episode.first_ns=begin;
      const auto meta=uint64_t{input.queue}|(uint64_t{input.stream}<<8)|
        (uint64_t{input.func}<<16)|(uint64_t{input.operation}<<24);
      ForwardRow(LabelCausalTrace::Stage::WaitSite,input.owner,input.submit,episode.id,
        input.address,input.width,input.ref,input.packet,meta);
      ForwardRow(LabelCausalTrace::Stage::WaitMask,input.owner,begin,episode.id,
        input.address,input.poll,input.mask,input.packet,input.submit);
    }
    ++episode.calls; episode.unpublished+=unpublished;
    episode.comparator_false+=!unpublished&&!satisfied;
    episode.poll_ns+=poll_elapsed; episode.read_ns+=read_elapsed;
    const bool transition=episode.last_path!=path ||
      (!unpublished&&(!episode.had_value||episode.last_value!=actual||satisfied));
    if(transition) {
      ForwardRow(LabelCausalTrace::Stage::WaitPollOutcome,input.owner,armed_tick,episode.id,
        input.address,poll_elapsed,static_cast<uint64_t>(path),input.packet,input.submit);
      if(!unpublished) ForwardRow(LabelCausalTrace::Stage::WaitReadResult,input.owner,read_elapsed,
        episode.id,input.address,input.width,actual,input.packet,satisfied?1:0);
    }
    episode.last_path=path;
    if(!unpublished) { episode.had_value=true; episode.last_value=actual; }
    if(!unpublished&&satisfied) {
      ForwardRow(LabelCausalTrace::Stage::WaitEpisodeDone,input.owner,input.submit,episode.id,
        input.address,episode.calls,episode.unpublished,episode.comparator_false,end-episode.first_ns);
      ForwardRow(LabelCausalTrace::Stage::WaitEpisodeTiming,input.owner,episode.poll_ns,episode.id,
        input.address,episode.read_ns,0,input.packet,input.submit);
      episode={};
    }
  }
};
inline void NotePoll(PollPath path,uint64_t tick=0) noexcept {
  if(current!=nullptr) { current->path=path; current->armed_tick=tick; }
}
inline uint64_t CvBegin(uint64_t owner,uint64_t generation,uint64_t blocked_mask) noexcept {
  if(!Active()) return 0;
  const auto begin=LabelCausalTrace::Now();
  std::lock_guard lock(mutex);
  if(!Active()||dumped) return 0;
  ForwardRow(LabelCausalTrace::Stage::WaitCvBegin,owner,generation,begin,0,0,blocked_mask);
  return begin;
}
inline void CvEnd(uint64_t owner,uint64_t begin,bool notified,uint64_t generation) noexcept {
  if(begin==0) return;
  const auto elapsed=LabelCausalTrace::Now()-begin;
  std::lock_guard lock(mutex);
  if(!Active()||dumped) return;
  ++cv_calls; cv_timeouts+=!notified; cv_signalled+=notified; cv_ns+=elapsed;
  cv_max_ns=std::max(cv_max_ns,elapsed);
  const auto bucket=elapsed<100000?0:elapsed<500000?1:elapsed<1000000?2:
    elapsed<2000000?3:elapsed<5000000?4:5;
  ++cv_histogram[bucket];
  ForwardRow(LabelCausalTrace::Stage::WaitCvEnd,owner,generation,begin,0,elapsed,notified?1:0);
}
inline void SourceNotify(uint64_t owner,uint64_t generation) noexcept {
  if(!Active()) return;
  std::lock_guard lock(mutex);
  if(!Active()||dumped) return;
  ++source_notifies;
  ForwardRow(LabelCausalTrace::Stage::WaitSourceNotify,owner,generation,0,0,0,source_notifies);
}
inline void DumpIfClosed() {
  if(!Enabled() || LabelCausalTrace::start_ns.load()==0 || LabelCausalTrace::active.load()) return;
  decltype(totals) counts;
  decltype(episodes) outstanding;
  uint64_t requested,recorded,suppressed,bad,superseded,calls,timeouts,signalled,elapsed,max_elapsed,notifies;
  decltype(cv_histogram) histogram;
  {
    std::lock_guard lock(mutex);
    if(dumped) return;
    dumped=true; counts=totals; outstanding=episodes; histogram=cv_histogram;
    requested=requested_rows; recorded=recorded_rows; suppressed=suppressed_rows;
    bad=bad_slots; superseded=superseded_episodes;
    calls=cv_calls; timeouts=cv_timeouts; signalled=cv_signalled;
    elapsed=cv_ns; max_elapsed=cv_max_ns; notifies=source_notifies;
  }
  std::printf("wait-poll end slots=%u row_limit=%" PRIu64 " requested_rows=%" PRIu64
    " forwarded_rows=%" PRIu64 " suppressed_rows=%" PRIu64 " bad_slots=%" PRIu64
    " superseded_episodes=%" PRIu64 " cv_calls=%" PRIu64 " cv_timeout=%" PRIu64
    " cv_signalled=%" PRIu64 " cv_ns=%" PRIu64 " cv_max_ns=%" PRIu64
    " source_notify=%" PRIu64 " cv_bins=%" PRIu64 ",%" PRIu64 ",%" PRIu64
    ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 " domain=steady_ns no_cpu_store_timestamp=1\n",
    SlotCount,RowLimit,requested,recorded,suppressed,bad,superseded,calls,timeouts,signalled,
    elapsed,max_elapsed,notifies,histogram[0],histogram[1],histogram[2],histogram[3],histogram[4],histogram[5]);
  for(uint32_t slot=0;slot<SlotCount;++slot) {
    const auto& c=counts[slot]; const auto& e=outstanding[slot];
    if(c.attempts==0) continue;
    std::printf("wait-poll queue=%u stream=%u attempts=%" PRIu64 " unpublished=%" PRIu64
      " comparator_false=%" PRIu64 " comparator_true=%" PRIu64 " poll_ns=%" PRIu64
      " read_ns=%" PRIu64 " max_poll_ns=%" PRIu64 " max_read_ns=%" PRIu64
      " paths=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
      " outstanding=%" PRIu64 " address=0x%" PRIx64 " packet=0x%" PRIx64
      " submit=%" PRIu64 " first_ns=%" PRIu64 " calls=%" PRIu64 "\n",
      slot/2,slot%2,c.attempts,c.unpublished,c.comparator_false,c.comparator_true,c.poll_ns,c.read_ns,
      c.max_poll_ns,c.max_read_ns,c.paths[0],c.paths[1],c.paths[2],c.paths[3],c.paths[4],c.paths[5],
      e.id,e.input.address,e.input.packet,e.input.submit,e.first_ns,e.calls);
  }
  std::fflush(stdout);
}
} // namespace Libs::Graphics::WaitPollDiagnostic
#endif
