// Actual public CommandStream scheduling/payload fixture. No Vulkan operation or
// synthetic GPU-ready flag: the routing value is only an opaque CPU test token.
#include "graphics/host_gpu/renderer/commandHooks.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <semaphore>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Libs::Graphics {
namespace {
namespace IdleStreamFixture {
using Phase=CommandStreamRegression::Phase;
struct Control {
  std::mutex mutex;std::condition_variable cv;
  Phase hold=Phase::BeforeSleepMark;
  bool arrived=false,released=false,timeout=false,captured=false,drain_waiting=false;
  uint32_t first_spins=0;std::vector<int32_t> order;
  bool payload=true,counters=false;
  uint64_t cycles=0;
  std::binary_semaphore done{0},drain_done{0},consumer_done{0};
};
thread_local Control* consumer_control=nullptr;
constexpr uintptr_t BufferToken=0x1230u;
struct Payload {uint32_t sequence;std::array<uint32_t,3> words;};
constexpr std::array<std::array<uint32_t,3>,3> Goldens{{
  {0x12345678u,0x9abcdef0u,0xc3a97153u},
  {0xfedcba98u,0x76543210u,0x1a7539c3u},
  {0x5371c3a9u,0x09abcdefu,0x87a9531cu}}};
void Observe(Phase phase,uint32_t spins,void* opaque) {
  auto& c=*static_cast<Control*>(opaque);std::unique_lock lock(c.mutex);
  if(phase==Phase::EmptySpinComplete&&!c.captured) {c.first_spins=spins;c.captured=true;}
  if(phase==Phase::DrainWaiting) {c.drain_waiting=true;c.cv.notify_all();}
  if(phase==c.hold&&!c.arrived) {
    c.arrived=true;c.cv.notify_all();
    if(!c.cv.wait_for(lock,std::chrono::seconds(2),[&]{return c.released;}))c.timeout=true;
  }
}
void Run(VkCommandBuffer buffer,const uint8_t* bytes) {
  auto& c=*consumer_control;Payload p{};std::memcpy(&p,bytes,sizeof(p));
  c.payload=c.payload&&reinterpret_cast<uintptr_t>(buffer)==BufferToken&&
    reinterpret_cast<uintptr_t>(bytes)%16u==0u&&p.sequence>=1u&&p.sequence<=3u;
  if(p.sequence>=1u&&p.sequence<=3u)c.payload=c.payload&&p.words==Goldens[p.sequence-1u];
  c.order.push_back(static_cast<int32_t>(p.sequence));
}
struct Result {bool arrived=false,completed=false,order=false,payload=false,timeout=false,counters=false;
  bool captured=false,drain_entered=true,drain_early=false,drain_completed=true;
  uint32_t spins=0;uint64_t cycles=0;};
Result RunCase(Phase phase,bool test_drain=false) {
  Control c;c.hold=phase;Result result;
  {
    CommandStream stream;
    const auto routed=reinterpret_cast<VkCommandBuffer>(BufferToken);stream.Route(routed);
    std::jthread consumer([&](std::stop_token stop) {
      consumer_control=&c;CommandStreamRegression::context=&c;CommandStreamRegression::observer=Observe;
      ULONG64 before=0,after=0;
      const bool first=QueryThreadCycleTime(GetCurrentThread(),&before)!=0;
      stream.Consume(stop);
      c.counters=first&&QueryThreadCycleTime(GetCurrentThread(),&after)!=0&&after>before;
      c.cycles=c.counters?after-before:0u;
      CommandStreamRegression::observer=nullptr;CommandStreamRegression::context=nullptr;consumer_control=nullptr;
      c.consumer_done.release();
    });
    {std::unique_lock lock(c.mutex);result.arrived=c.cv.wait_for(lock,std::chrono::seconds(2),[&]{return c.arrived;});}
    // Three small payloads plus callbacks stay far below automatic16KiB wake.
    // The producer really publishes while the consumer is held at the selected
    // before/after-sleep mark or just before atomic::wait. Wake is an actual API.
    if(result.arrived) {
      for(uint32_t sequence=1;sequence<=3u;++sequence) {
        Payload p{sequence,Goldens[sequence-1u]};auto* bytes=ReserveRecordedCall(routed,sizeof(p));
        Require("CommandStreamIdleSpin","public payload reservation",bytes!=nullptr,
          "actual routing owner refused the payload before the spin cost oracle");
        std::memcpy(bytes,&p,sizeof(p));CommitRecordedCall(Run);
        stream.Push([&c,sequence]{c.order.push_back(-static_cast<int32_t>(sequence));});
      }
      stream.Push([&c]{c.done.release();});stream.Wake();
    }
    std::unique_ptr<std::jthread> drainer;
    if(result.arrived&&test_drain) {
      drainer=std::make_unique<std::jthread>([&] {
        CommandStreamRegression::context=&c;CommandStreamRegression::observer=Observe;
        stream.Drain();
        CommandStreamRegression::observer=nullptr;CommandStreamRegression::context=nullptr;
        c.drain_done.release();
      });
      {std::unique_lock lock(c.mutex);result.drain_entered=c.cv.wait_for(lock,std::chrono::seconds(1),[&]{return c.drain_waiting;});}
      result.drain_early=c.drain_done.try_acquire_for(std::chrono::milliseconds(25));
    }
    {std::lock_guard lock(c.mutex);c.released=true;}c.cv.notify_all();
    result.completed=result.arrived&&c.done.try_acquire_for(std::chrono::seconds(2));
    if(result.arrived&&!result.completed) {
      // Cleanup may give this owned stream one extra real wake. It preserves
      // completed=false, so a lost-wake failure can never become the cost RED
      // or a functional PASS merely because cleanup rescued it.
      stream.Wake();(void)c.done.try_acquire_for(std::chrono::milliseconds(250));
    }
    if(drainer)result.drain_completed=result.drain_early||c.drain_done.try_acquire_for(std::chrono::seconds(2));
    // A failed wake is retained as a functional failure. Requesting stop wakes
    // only this owned consumer for cleanup, never turns that failure into PASS.
    consumer.request_stop();
    if(!c.consumer_done.try_acquire_for(std::chrono::seconds(2)) ||
       (drainer&&!result.drain_completed)) {
      // PureCPU fixture: terminate its own process at the finite retirement
      // deadline rather than join a broken producer/consumer protocol forever.
      // No GPU/device was created. This is a functional/setup failure, NOT the
      // intended completed-spins cost reproduction.
      std::fprintf(stderr,"CommandStreamIdleSpin functional retirement deadline; NOT cost RED\n");
      std::abort();
    }
    consumer.join();if(drainer)drainer->join();
    if(result.completed)stream.Drain(); // bounded prerequisite: all queued calls already ran.
    stream.Route(VK_NULL_HANDLE);
    result.order=c.order==std::vector<int32_t>{1,-1,2,-2,3,-3};result.payload=c.payload;
    result.timeout=c.timeout;result.counters=c.counters;result.captured=c.captured;
    result.spins=c.first_spins;result.cycles=c.cycles;
  } // real CommandStream asserts read==write; consumer joined and routing closed.
  return result;
}
}
void CheckCommandStreamIdleSpinCost(bool cost) {
  constexpr const char* name="CommandStreamIdleSpin";
  const char* old_raw=std::getenv("KYTY_RECORD_SPIN_ITERATIONS");const std::string old=old_raw!=nullptr?old_raw:"";
  Require(name,"optional process-only idle budget",_putenv_s("KYTY_RECORD_SPIN_ITERATIONS","100")==0,
      "could not install explicit idle-spin planning input; no production getter required in baseline");
  constexpr std::array phases{IdleStreamFixture::Phase::BeforeSleepMark,
    IdleStreamFixture::Phase::AfterSleepMark,IdleStreamFixture::Phase::BeforeSignalWait,
    IdleStreamFixture::Phase::BeforeSignalWait};
  std::array<IdleStreamFixture::Result,4> results;
  // The3 lost-wake controls use only the producer's Wake. A separate fourth
  // case proves Drain really waits for withheld work; its extra wake cannot
  // accidentally rescue or conceal a failure of the preceding3 controls.
  for(size_t i=0;i<results.size();++i)results[i]=IdleStreamFixture::RunCase(phases[i],i==3u);
  const bool restored=_putenv_s("KYTY_RECORD_SPIN_ITERATIONS",old.c_str())==0;
  for(size_t i=0;i<results.size();++i) {
    const auto& r=results[i];
    std::printf("[command-stream-idle] case=%zu phase=%u arrived=%u completed=%u order=%u payload=%u "
        "timeout=%u CPUcounter=%u captured=%u drainEntered=%u drainEarly=%u drainComplete=%u "
        "actualSpins=%u rawCycles=%llu cleanup=joined+closed\n",
        i,static_cast<uint32_t>(phases[i]),r.arrived,r.completed,r.order,r.payload,r.timeout,r.counters,
        r.captured,r.drain_entered,r.drain_early,r.drain_completed,r.spins,
        static_cast<unsigned long long>(r.cycles));
    Require(name,"real small-batch/order/alignment/wake/lifetime controls",r.arrived&&r.completed&&
        r.order&&r.payload&&!r.timeout&&r.counters&&r.captured&&r.drain_entered&&!r.drain_early&&r.drain_completed&&restored,
        "actual stream scheduling/payload/counter/env/cleanup failed before optional spin-count cost");
  }
  if(cost)for(const auto& r:results)Require(name,"explicit idle spin budget",r.spins<=100u,
      "actual empty-ring consumer executed more than the explicit100-iteration budget");
}
} // namespace
} // namespace Libs::Graphics
