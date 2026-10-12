#include "graphics/host_gpu/renderer/writerHistory.h"
#include <cstdio>

using namespace Libs::Graphics::WriterHistory;
static bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "writer history: %s\n", message);
    return condition;
}
int main() {
    History h(4);
    Record a; a.address = 0x1000; a.size = 0x100; a.shader = 0x111;
    a.kind = Kind::ComputeBuffer; a.slot = 3;
    if (!Check(h.Add(a), "valid writer rejected")) return 1;
    a.address = 0x2000; a.shader = 0x222;
    if (!Check(h.Add(a), "second writer rejected")) return 1;
    const auto cutoff = h.Snapshot().last_sequence;
    a.address = 0x1000; a.shader = 0x333;
    h.Add(a);
    auto q = h.Read(0x1080, 0x10, cutoff);
    if (!Check(q.matches == 1 && q.records[0].shader == 0x111 &&
               q.records[0].slot == 3, "reader cutoff admitted a later writer")) return 1;
    q = h.Read(0x1100, 0x100, h.Snapshot().last_sequence);
    if (!Check(q.matches == 0, "adjacent ranges falsely overlap")) return 1;
    if (!Check(!h.Add(Record{}) && !h.Read(0x1000, 0, cutoff).valid_range,
               "null/empty range accepted")) return 1;
    Record bad; bad.address = UINT64_MAX - 3; bad.size = 4;
    if (!Check(!h.Add(bad) && !h.Read(bad.address, bad.size, cutoff).valid_range,
               "overflowing range accepted")) return 1;
    a.shader = 0x444; h.Add(a);
    a.shader = 0x555; h.Add(a);
    q = h.Read(0x1000, 0x100, h.Snapshot().last_sequence, 2);
    if (!Check(q.stats.retained == 4 && q.stats.dropped == 1 &&
               q.stats.first_retained_sequence == 2 && q.stats.rejected == 2 &&
               q.matches == 3 && q.truncated && q.records.size() == 2 &&
               q.records[0].shader == 0x555 && q.records[1].shader == 0x444,
               "ring wrap/match cap lost latest writers or coverage metadata")) return 1;
    q = h.Read(0x1000, 0x100, cutoff);
    if (!Check(q.matches == 0 && q.stats.dropped == 1,
               "evicted earlier writer invented or coverage hidden")) return 1;
    a.address = 0x3000; a.shader = 0x666; h.Add(a);
    q = h.Read(0x3000, 4, h.Snapshot().last_sequence);
    if (!Check(q.matches == 1 && q.records[0].shader == 0x666,
               "dynamic reader address did not select relocated writer")) return 1;
    q = h.Read(0x3000, 4, h.Snapshot().last_sequence, 0);
    if (!Check(q.matches == 1 && q.records.empty() && q.truncated,
               "zero output budget hid known matching writer")) return 1;
    History one(0); a.address = UINT64_MAX - 4; a.size = 4;
    if (!Check(one.Add(a) && one.Read(a.address, a.size, 1).matches == 1,
               "last representable exclusive endpoint or minimum capacity failed")) return 1;
    History producers(80);
    a.address = 0x4000; a.size = 0x100;
    for (uint64_t i = 1; i <= 70; ++i) { a.shader = i; producers.Add(a); }
    q = producers.Read(0x4000, 4, 70);
    if (!Check(q.records.size() == 8 && q.matches == 70 && q.truncated &&
               q.records.front().shader == 70 && q.records.back().shader == 63,
               "default newest-eight view changed")) return 1;
    q = producers.Read(0x4000, 4, 28, 64);
    if (!Check(q.matches == 28 && q.records.size() == 28 && !q.truncated &&
               q.records.front().shader == 28 && q.records.back().shader == 1,
               "expanded cutoff view lost older producer declarations")) return 1;
    q = producers.Read(0x4000, 4, 70, SIZE_MAX);
    if (!Check(q.matches == 70 && q.records.size() == 64 && q.truncated &&
               q.records.back().shader == 7,
               "oversized query escaped the diagnostic output bound")) return 1;
    std::puts("writer history: CPU boundary/cutoff/relocation/wrap tests PASS");
    return 0;
}
