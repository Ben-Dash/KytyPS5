// CPU-only regression through the real exported guest ABI. No mirrored HLE.
#include "libs/errno.h"
#include "loader/symbolDatabase.h"

namespace Libs {
void InitUserService_1(Loader::SymbolDatabase*);
}

namespace Libs::Graphics {
namespace {
void CheckUserServiceGamePresetPriority(Config::ConfigOptions options) {
  constexpr const char* Name = "UserServiceGamePresetPriority";
  Loader::SymbolDatabase symbols;
  Libs::InitUserService_1(&symbols);
  const auto* symbol = symbols.Find({"-sD02mFDBh4", "UserService", 1,
                                    "UserService", 1, 1, Loader::SymbolType::Func});
  Require(Name, "actual guest export", symbol != nullptr && symbol->vaddr != 0,
          "actual UserService1 / module1.1 preset NID must resolve");
  using GuestPresets = int (KYTY_SYSV_ABI *)(int, void*);
  const auto get = reinterpret_cast<GuestPresets>(symbol->vaddr);
  constexpr uint64_t Sentinel = 0xd3f08a249167be5cull;
  constexpr size_t KnownBytes = 40;
  constexpr size_t PriorityOffset = 12;
  struct alignas(8) Output {
    uint64_t before[2];
    alignas(8) std::array<uint8_t, 64> bytes;
    uint64_t after[2];
  };
  auto prepare = [&](size_t requested) {
    Output out{};
    out.before[0] = out.before[1] = out.after[0] = out.after[1] = Sentinel;
    out.bytes.fill(0xa5);
    std::memcpy(out.bytes.data(), &requested, sizeof(requested));
    return out;
  };
  auto protected_edges = [&](const Output& out) {
    return out.before[0] == Sentinel && out.before[1] == Sentinel &&
           out.after[0] == Sentinel && out.after[1] == Sentinel;
  };
  unsigned mismatches = 0;
  for (const uint32_t requested_priority : {0u, 1u, 2u}) {
    options.game_preset_priority = requested_priority;
    Config::Load(options);
    Require(Name, "config plumbing", Config::GetGamePresetPriority() == requested_priority,
            "configuration must be present before testing unchanged HLE behavior");
    auto invalid = prepare(48);
    const auto invalid_before = invalid;
    Require(Name, "invalid user", get(options.user_id + 1, invalid.bytes.data()) ==
                UserService::USER_SERVICE_ERROR_NOT_LOGGED_IN &&
                std::memcmp(&invalid, &invalid_before, sizeof(invalid)) == 0,
            "invalid user preserves all output bytes and the existing error");
    Require(Name, "null output", get(options.user_id, nullptr) ==
                UserService::USER_SERVICE_ERROR_INVALID_ARGUMENT,
            "null preserves the actual HLE invalid-argument result");

    for (const size_t requested_size : {size_t{40}, size_t{48}, size_t{16}, size_t{8}, size_t{0}}) {
      auto out = prepare(requested_size);
      const size_t supported_size = requested_size == 0 || requested_size > KnownBytes ?
                                      KnownBytes : requested_size;
      Require(Name, "actual call", get(options.user_id, out.bytes.data()) == 0,
              "real configured-user preset export must return success");
      size_t returned_size = 0;
      std::memcpy(&returned_size, out.bytes.data(), sizeof(returned_size));
      bool neighbors = protected_edges(out) && returned_size == KnownBytes;
      for (size_t i = 8; i < supported_size; ++i) {
        if (i >= PriorityOffset && i < PriorityOffset + sizeof(uint32_t)) continue;
        neighbors = neighbors && out.bytes[i] == 0;
      }
      for (size_t i = supported_size; i < out.bytes.size(); ++i) {
        neighbors = neighbors && out.bytes[i] == 0xa5;
      }
      Require(Name, "legacy ABI and guards", neighbors,
              "known40 bytes,48 caller tail, smaller capacities and other fields remain unchanged");
      uint32_t observed = 0xa5a5a5a5u;
      if (supported_size >= PriorityOffset + sizeof(uint32_t)) {
        std::memcpy(&observed, out.bytes.data() + PriorityOffset, sizeof(observed));
        mismatches += observed != requested_priority;
      }
      std::printf("preset priority cfg=%u request=%zu supported=%zu observed=%u guards=PASS\n",
                  requested_priority, requested_size, supported_size, observed);
    }
  }
  // Every semantic/bootstrap/error/capacity control above runs before the intended RED.
  Require(Name, "configured guest priority", mismatches == 0,
          "real preset priority must honor shared Default0 / Performance1 / Resolution2 config");
  std::puts("UserServiceGamePresetPriority: all real exported ABI controls PASS");
}
} // namespace
} // namespace Libs::Graphics
