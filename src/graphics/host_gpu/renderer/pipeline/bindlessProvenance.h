#pragma once
#include <array>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Libs::Graphics::BindlessProvenance {
inline bool Enabled() {
    static const bool on = [] { const char* v = std::getenv("KYTY_BINDLESS_PROVENANCE"); return v && std::strcmp(v, "0") != 0; }();
    return on;
}
// Independent bounded stdout records:256 roots/links +32 failures, each <=2112bytes (<1MiB).
inline void Line(bool failure, const char* kind, const char* format, ...) {
    if (!Enabled()) return;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
#endif
    char text[2048]{};
    va_list args; va_start(args, format); const int n = std::vsnprintf(text, sizeof(text), format, args); va_end(args);
    if (n < 0) return;
    for (char* p = text; *p; ++p) if (*p == '\n' || *p == '\r') *p = ' ';
    static std::mutex mutex;
    static uint32_t roots = 0, failures = 0;
    static bool root_full = false, failure_full = false;
    static std::array<std::array<char,2048>,256> seen{};
    std::lock_guard lock(mutex);
    if (!failure) {
        for (uint32_t i = 0; i < roots; ++i) if (std::strcmp(seen[i].data(), text) == 0) return;
        if (roots == seen.size()) { if (!root_full) { root_full=true; std::puts("BindlessProvenance: root_link_budget_exhausted original_root_coverage_incomplete=1"); } return; }
        std::memcpy(seen[roots++].data(), text, sizeof(text));
    } else {
        if (failures == 32u) { if (!failure_full) { failure_full=true; std::puts("BindlessProvenance: requested_failure_budget_exhausted"); } return; }
        ++failures;
    }
    std::printf("BindlessProvenance: kind=%s clipped=%d %s\n", kind, n >= static_cast<int>(sizeof(text)), text);
    std::fflush(stdout);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    } catch (...) { } // Diagnostic failure must never replace a guest result or error.
#endif
}
struct ReadTrace { uint64_t pointer_address=0, pointer=0, target=0; bool pointer_ok=false, target_ok=false; std::array<uint32_t,8> words{}; };
// Group repeated null/unreadable rows without their changing key/address/epoch.
// Eight unavailable-input examples cannot consume the24 readable-payload reservations.
inline bool AdmitFailure(const char* key, bool payload) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
#endif
    static std::mutex mutex;
    static std::array<std::array<char,512>,32> seen{};
    static uint32_t count=0, unavailable=0, readable=0;
    static bool full_unavailable=false, full_readable=false;
    std::lock_guard lock(mutex);
    for (uint32_t i=0;i<count;++i) if (std::strcmp(seen[i].data(),key)==0) return false;
    auto& used=payload ? readable : unavailable; auto& reported=payload ? full_readable : full_unavailable;
    if (used==(payload ? 24u : 8u)) { if (!reported) { reported=true; std::printf("BindlessProvenance: failure_diversity_budget_exhausted readable_payload=%d\n",payload); std::fflush(stdout); } return false; }
    std::snprintf(seen[count++].data(),seen[0].size(),"%s",key); ++used; return true;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    } catch (...) { return false; }
#endif
}
inline void Words(char (&out)[80], const ReadTrace& r) {
    if (!r.target_ok) { std::snprintf(out,sizeof(out),"unavailable"); return; }
    std::snprintf(out,sizeof(out),"%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",r.words[0],r.words[1],r.words[2],r.words[3],r.words[4],r.words[5],r.words[6],r.words[7]);
}
} // namespace Libs::Graphics::BindlessProvenance
