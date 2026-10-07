#ifndef LAUNCHER_INCLUDE_EMULATORSWITCHES_H_
#define LAUNCHER_INCLUDE_EMULATORSWITCHES_H_

#include <array>

// Emulator switches that are off by default and read from the environment (KYTY_GETENV): a game
// profile can turn them on, and the launcher then sets them to "1" for that game's emulator
// process. A variable already set in the launcher's own environment (a .bat, the system) is
// passed on unchanged, so the environment keeps working as before. To offer a new switch, add a
// row here; the profile dialog and the launch pick it up from this table. Rows are sorted by the
// expected gain, largest first, and the texts are written for players, not developers.
struct EmulatorSwitch {
	const char* name;     // Environment variable, also the key stored in the profile
	const char* label;    // Check box text in the profile dialog
	const char* tooltip;  // What it does, in plain words
	const char* measured; // What it did in a measurement, or what it is for
};

inline constexpr std::array<EmulatorSwitch, 5> EMULATOR_SWITCHES = {{
    {"KYTY_TRACKING_LOCK_BACKOFF", "Calmer waiting on memory checks (not recommended)",
     "When many game threads touch the same memory at once, they queue up and keep asking whether "
     "it is their turn, which keeps the processor busy with waiting. With this on, waiting threads "
     "pause between questions and let other work run. In theory the largest gain of these "
     "switches, because the waiting used up almost half of the processor time in one test.",
     "Measured in ASTRO BOT (Sky Garden): about 14 fps instead of 25, much slower. Leave it off."},
    {"KYTY_UNMAP_SKIP_DRAIN", "Skip needless waits when memory is freed",
     "When a game frees memory, the emulator normally waits until the graphics card has finished "
     "all its work. With this on, it skips that wait when the graphics card never used that "
     "memory. Can remove short hitches.",
     "Measured in ASTRO BOT (Sky Garden): 25.7 fps instead of 25, within normal variation. At most "
     "fewer hitches."},
    {"KYTY_DYNAMIC_DEPTH_BOUNDS", "Fewer graphics setups (depth range)",
     "Draws that differ only in one depth setting share one prepared graphics setup instead of "
     "each getting its own. That means fewer setups to prepare while playing. The first start "
     "with this on prepares the setups again once, so expect a slow first start.",
     "Measured in ASTRO BOT (Sky Garden): about 9 % slower. Leave it off for now."},
    {"KYTY_DEBUG_DRAW_REPEAT", "Count repeating draws (diagnostics)",
     "Does not make anything faster. Every 120 frames the console window shows how many drawing "
     "steps look exactly like one from the last frames. That tells whether reusing them would "
     "help in this game.",
     "Gain: none, it only measures. In ASTRO BOT no draw repeated exactly, because the game moves "
     "its data every frame."},
    {"KYTY_PROTECT_EXEC_QUERY", "Old memory protection check (not recommended)",
     "Brings back the old, slower way of checking whether game code may run from a piece of "
     "memory: the emulator asks Windows each time instead of remembering it. Only there to "
     "compare against the new way if a game misbehaves.",
     "Measured in ASTRO BOT (Sky Garden): about 23 fps instead of 37, much slower. Leave it off."},
}};

#endif /* LAUNCHER_INCLUDE_EMULATORSWITCHES_H_ */
