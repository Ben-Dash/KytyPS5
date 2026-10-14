// Pure CPU production-hook/real-CommandStream fixture, before any Vulkan initialization.
// Opaque handles are caller tokens; no native GPU object or driver call is created.
#include "graphics/host_gpu/renderer/commandHooks.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

namespace Libs::Graphics {
namespace {
namespace DrawIndirectStreamFixture {
struct Control {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<uint32_t> order;
  bool entered=false,released=false,returned=false,gate_timeout=false,releaser_timeout=false;
  bool gate_complete=false,forward_after_gate=true;
  uint32_t drains=0,calls=0;
  bool arguments=true;
  std::binary_semaphore payload_done{0},consumer_done{0},releaser_done{0};
};
inline Control* spy_control=nullptr;
constexpr uintptr_t CommandToken=0x1230u,ArgumentToken=0x4560u;
constexpr VkDeviceSize ExpectedOffset=0x100000004ull;
constexpr uint32_t ExpectedCount=7u,ExpectedStride=32u;
void VKAPI_PTR ForwardedDraw(VkCommandBuffer buffer,VkBuffer args,VkDeviceSize offset,
                            uint32_t count,uint32_t stride) {
  auto& c=*spy_control;
  std::lock_guard lock(c.mutex);
  ++c.calls;
  c.forward_after_gate&=c.gate_complete;
  c.arguments&=reinterpret_cast<uintptr_t>(buffer)==CommandToken&&
      reinterpret_cast<uintptr_t>(args)==ArgumentToken&&offset==ExpectedOffset&&
      count==ExpectedCount&&stride==ExpectedStride;
  c.order.push_back(2u);
}
void Observe(CommandStreamRegression::Phase phase,uint32_t,void* opaque) {
  if(phase!=CommandStreamRegression::Phase::DrainWaiting)return;
  auto& c=*static_cast<Control*>(opaque);
  std::lock_guard lock(c.mutex);
  ++c.drains;c.cv.notify_all();
}
}
void CheckCommandStreamDrawIndirectRecording() {
  constexpr const char* name="CommandStreamDrawIndirectRecording";
  using namespace DrawIndirectStreamFixture;
  const char* old_record_raw=std::getenv("KYTY_RECORD_THREAD");
  const char* old_copy_raw=std::getenv("KYTY_DRAW_INDIRECT_RECORD");
  const std::string old_record=old_record_raw!=nullptr?old_record_raw:"";
  const std::string old_copy=old_copy_raw!=nullptr?old_copy_raw:"";
  Require(name,"fresh process recording profile",_putenv_s("KYTY_RECORD_THREAD","1")==0&&
      _putenv_s("KYTY_DRAW_INDIRECT_RECORD","1")==0,
      "could not configure before first hook install; not intended RED");
  auto& dispatcher=VULKAN_HPP_DEFAULT_DISPATCHER;
  const auto previous=dispatcher.vkCmdDrawIndirect;
  dispatcher.vkCmdDrawIndirect=ForwardedDraw;
  InstallCommandHooks();
  Require(name,"actual production recording hooks",CommandRecordingDeferred()&&
      dispatcher.vkCmdDrawIndirect!=ForwardedDraw,
      "fresh standalone hook setup failed; not intended RED");
  Control c;spy_control=&c;
  bool entered=false,payload_complete=false,consumer_joined=false,releaser_joined=false;
  bool stream_drained=false,order=false,arguments=false,no_timeouts=false,forward_after_gate=false;
  uint32_t calls=0,drains=0;
  {
    CommandStream stream;
    const auto command=reinterpret_cast<VkCommandBuffer>(CommandToken);
    stream.Route(command);
    std::jthread consumer([&](std::stop_token stop) {
      stream.Consume(stop);
      c.consumer_done.release();
    });
    stream.Push([&] {
      std::unique_lock lock(c.mutex);
      c.order.push_back(1u);c.entered=true;c.cv.notify_all();
      if(!c.cv.wait_for(lock,std::chrono::seconds(2),[&]{return c.released;}))
        c.gate_timeout=true;
      c.gate_complete=true;
    });
    stream.Wake();
    {
      std::unique_lock lock(c.mutex);
      entered=c.cv.wait_for(lock,std::chrono::seconds(1),[&]{return c.entered;});
    }
    // Release at a REAL production Drain boundary on RED, or the real producer
    // return on GREEN. No timer value or thread scheduling speed is the cost oracle.
    std::jthread releaser([&] {
      std::unique_lock lock(c.mutex);
      if(!c.cv.wait_for(lock,std::chrono::seconds(2),[&]{return c.drains!=0u||c.returned;}))
        c.releaser_timeout=true;
      c.released=true;c.cv.notify_all();
      c.releaser_done.release();
    });
    if(entered) {
      CommandStreamRegression::context=&c;CommandStreamRegression::observer=Observe;
      dispatcher.vkCmdDrawIndirect(command,reinterpret_cast<VkBuffer>(ArgumentToken),
          ExpectedOffset,ExpectedCount,ExpectedStride);
      CommandStreamRegression::observer=nullptr;CommandStreamRegression::context=nullptr;
      {
        std::lock_guard lock(c.mutex);c.returned=true;c.cv.notify_all();
      }
      stream.Push([&] {
        {std::lock_guard lock(c.mutex);c.order.push_back(3u);}
        c.payload_done.release();
      });
      stream.Wake();
      payload_complete=c.payload_done.try_acquire_for(std::chrono::seconds(2));
    } else {
      std::lock_guard lock(c.mutex);c.released=true;c.cv.notify_all();
    }
    consumer.request_stop();
    consumer_joined=c.consumer_done.try_acquire_for(std::chrono::seconds(2));
    releaser_joined=c.releaser_done.try_acquire_for(std::chrono::seconds(2));
    if(!consumer_joined||!releaser_joined) {
      std::fprintf(stderr,"draw-indirect CPU worker retirement deadline; NOT cost RED\n");
      std::abort();
    }
    consumer.join();releaser.join();
    // Consumer exit proves it drained all published entries; the public Drain
    // consequently cannot wait on a withheld worker. The destructor checks read==write.
    stream.Drain();stream.Route(VK_NULL_HANDLE);stream_drained=true;
    std::lock_guard lock(c.mutex);
    order=c.order==std::vector<uint32_t>{1u,2u,3u};arguments=c.arguments;
    forward_after_gate=c.forward_after_gate;
    no_timeouts=!c.gate_timeout&&!c.releaser_timeout;calls=c.calls;drains=c.drains;
  }
  spy_control=nullptr;dispatcher.vkCmdDrawIndirect=previous;
  const bool environment_restored=_putenv_s("KYTY_RECORD_THREAD",old_record.c_str())==0&&
      _putenv_s("KYTY_DRAW_INDIRECT_RECORD",old_copy.c_str())==0;
  std::printf("[draw-indirect-record] hostOnly=1 productionHook=1 realStream=1 entered=%u "
      "complete=%u calls=%u fiveArguments=%u order=%u forwardAfterGate=%u nativeDriverCalls=0 "
      "consumerJoined=%u releaserJoined=%u streamDrained=%u timeouts=%u envRestored=%u "
      "actualProducerDrains=%u\n",entered,payload_complete,calls,arguments,order,forward_after_gate,
      consumer_joined,releaser_joined,stream_drained,!no_timeouts,environment_restored,drains);
  std::fflush(stdout);
  Require(name,"all forwarding/order/bounded worker/cleanup controls BEFORE cost oracle",
      entered&&payload_complete&&calls==1u&&arguments&&order&&forward_after_gate&&consumer_joined&&releaser_joined&&
      stream_drained&&no_timeouts&&environment_restored,
      "functional/setup/retirement failure; not the producer-drain cost RED");
  Require(name,"copyable indirect command admits without producer drain",drains==0u,
      "vkCmdDrawIndirect drained the producer despite five copyable arguments and ordered recorder");
}
} // namespace
} // namespace Libs::Graphics
