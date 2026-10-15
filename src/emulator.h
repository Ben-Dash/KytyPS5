#ifndef EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_
#define EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_

#include "common/emulatorConfig.h"
#include "common/stringUtils.h"

#include <filesystem>

namespace Common { class Subsystems; }

namespace Emulator {

struct RunOptions {
	Config::ConfigOptions config;
	std::filesystem::path app0_dir;
	std::filesystem::path elf;
	std::filesystem::path game_patch;
};

void Run(const RunOptions& options);

// Shared native startup after Config/Log and guest memory-size configuration.
// Also exercised by the bounded host-only startup regression (no ELF or game).
void InitializeHostSubsystems(Common::Subsystems& subsystems);

} // namespace Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_ */
