// Real bounded CPU probe for diagnostic accounting. No GPU/guest emulation claim.
#include "graphics/host_gpu/renderer/functionProfile.h"
namespace Libs::Graphics {
namespace {
void CheckFunctionProfileSmoke() {
  namespace P=FunctionProfile;
  constexpr const char* name="FunctionProfileSmoke";
  P::Configure(true,1);P::SetRole(P::Role::Main);P::TickFrame(650);
  uint32_t actual=0;
  {
    P::Scope parent(P::Site::GetGraphicsPrograms);
    {
      P::Scope child(P::Site::FindImage);
      volatile uint32_t work=0;
      for(uint32_t i=0;i<100000;++i)work=work+i;
      actual=work;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::jthread worker([]{
    P::SetRole(P::Role::Submit);P::Scope scope(P::Site::Submit);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });worker.join();
  auto& parent=P::tls_rows->rows[static_cast<uint32_t>(P::Site::GetGraphicsPrograms)];
  auto& child=P::tls_rows->rows[static_cast<uint32_t>(P::Site::FindImage)];
  const auto parent_wall=parent.wall_inclusive_ns.load(),child_wall=child.wall_inclusive_ns.load();
  const auto parent_cpu=parent.cpu_inclusive_cycles.load(),child_cpu=child.cpu_inclusive_cycles.load();
  Require(name,"real CPU work",actual==static_cast<uint32_t>(uint64_t{99999}*100000/2),"bounded body output changed");
  Require(name,"selected nested counts and exact same-thread exclusive split",
    P::registered.load()==2&&parent.calls.load()==1&&child.calls.load()==1&&parent.sampled.load()==1&&child.sampled.load()==1&&
    parent.wall_done.load()==1&&child.wall_done.load()==1&&parent_wall>=child_wall&&
    parent.wall_exclusive_ns.load()+child_wall==parent_wall&&parent.outstanding.load()==0&&child.outstanding.load()==0,
    "nested/counter/thread accounting failed");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  Require(name,"actual raw cycle queries, nested split and empty-probe bookkeeping",
    parent.cpu_done.load()==1&&child.cpu_done.load()==1&&parent.cpu_exclusive_done.load()==1&&parent_cpu>=child_cpu&&
    parent.cpu_exclusive_cycles.load()+child_cpu==parent_cpu&&parent.query_errors.load()==0&&child.query_errors.load()==0&&
    parent.empty_probe_samples.load()==1&&child.empty_probe_samples.load()==0&&parent.query_count.load()==4&&child.query_count.load()==2,
    "CPU cycles/error/probe bookkeeping failed");
#endif
  P::Configure(false);
  {P::Scope disabled(P::Site::GetGraphicsPrograms);}
  Require(name,"disabled means no extra samples",P::active_epoch.load()==0&&parent.calls.load()==1,"disabled collector recorded work");
  std::printf("[function-profile-smoke] nativeWork=PASS threads=2 nestedWall=PASS nestedRawCycles=PASS emptyProbe=PASS disabled=PASS outstanding=0 parentWallNs=%llu childWallNs=%llu parentRawCycles=%llu childRawCycles=%llu\n",
    static_cast<unsigned long long>(parent_wall),static_cast<unsigned long long>(child_wall),
    static_cast<unsigned long long>(parent_cpu),static_cast<unsigned long long>(child_cpu));
}
}
}
