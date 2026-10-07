#ifndef LAUNCHER_INCLUDE_EMULATORSWITCHES_H_
#define LAUNCHER_INCLUDE_EMULATORSWITCHES_H_

#include <array>

// Emulator switches that are off by default and read from the environment (KYTY_GETENV): a game
// profile can turn them on, and the launcher then sets them to "1" for that game's emulator
// process. A variable already set in the launcher's own environment (a .bat, the system) is
// passed on unchanged, so the environment keeps working as before. To offer a new switch, add a
// row here; the profile dialog and the launch pick it up from this table.
struct EmulatorSwitch {
	const char* name;    // Environment variable, also the key stored in the profile
	const char* label;   // Check box text in the profile dialog
	const char* tooltip; // What it does
};

inline constexpr std::array<EmulatorSwitch, 4> EMULATOR_SWITCHES = {{
    {"KYTY_UNMAP_SKIP_DRAIN", "Skip GPU drain on CPU-only unmaps",
     "An unmap of memory that no buffer or image owns does not wait for the GPU (upstream "
     "e6fce45d)."},
    {"KYTY_DYNAMIC_DEPTH_BOUNDS", "Dynamic depth bounds",
     "Depth-bounds test and range are dynamic state, so pipelines that differ only in them are "
     "shared (upstream 98401022). The driver shader cache misses once after switching it on."},
    {"KYTY_TRACKING_LOCK_BACKOFF", "Tracking lock backoff",
     "Backoff while waiting for the memory tracking lock (Recherche thread, measurement pending)."},
    {"KYTY_DEBUG_DRAW_REPEAT", "Draw repeat counter (diagnostics)",
     "Every 120 frames the console shows how many draws repeat from recent frames."},
}};

#endif /* LAUNCHER_INCLUDE_EMULATORSWITCHES_H_ */
