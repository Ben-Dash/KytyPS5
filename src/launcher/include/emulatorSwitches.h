#ifndef LAUNCHER_INCLUDE_EMULATORSWITCHES_H_
#define LAUNCHER_INCLUDE_EMULATORSWITCHES_H_

#include <QString>
#include <QStringList>

#include <array>

// Emulator switches read from the environment (KYTY_GETENV): a game profile can turn them on, and
// the launcher then sets them to "1" for that game's emulator process. A switch can be on by
// default for some titles (default_titles, which the emulator's title profile also turns on);
// the check box then starts checked there, and unchecking it sets the variable to "0". A variable
// already set in the launcher's own environment (a .bat, the system) is passed on unchanged, so
// the environment keeps working as before. To offer a new switch, add a
// row here; the profile dialog and the launch pick it up from this table. Rows are sorted by the
// expected gain, largest first, and the texts are written for players, not developers.
struct EmulatorSwitch {
	const char* name;    // Environment variable, also the key stored in the profile
	const char* label;   // Check box text in the profile dialog
	const char* tooltip; // What it does, in plain words
	const char* verdict; // How it did in our tests, kept general: games react differently
	const char* default_titles = nullptr; // Title ids (comma separated) where it is on by default
};

inline constexpr std::array<EmulatorSwitch, 8> EMULATOR_SWITCHES = {{
    {"KYTY_COMPUTE_CHAIN", "Chain compute work without extra waits",
     "Compute steps that follow each other run without a full wait between them, as long as the "
     "game itself does not ask to wait in between. On by default for ASTRO BOT, ASTRO's PLAYROOM "
     "and Ghost of Yotei; uncheck it to turn it off for that game.",
     "In our tests clearly faster in Ghost of Yotei and the ASTRO games. If a game shows new "
     "graphics errors, try it off.",
     "PPSA21564,PPSA01325,PPSA26344"},
    {"KYTY_TRACKING_LOCK_BACKOFF", "Calmer waiting on memory checks (not recommended)",
     "When many game threads touch the same memory at once, they queue up and keep asking whether "
     "it is their turn, which keeps the processor busy with waiting. With this on, waiting threads "
     "pause between questions and let other work run. In theory the largest gain of these "
     "switches.",
     "In our tests this was much slower. May help some games."},
    {"KYTY_UNMAP_SKIP_DRAIN", "Skip needless waits when memory is freed",
     "When a game frees memory, the emulator normally waits until the graphics card has finished "
     "all its work. With this on, it skips that wait when the graphics card never used that "
     "memory. Can remove short hitches.",
     "In our tests no clear difference. May reduce hitches in some games."},
    {"KYTY_DYNAMIC_DEPTH_BOUNDS", "Fewer graphics setups (depth range)",
     "Draws that differ only in one depth setting share one prepared graphics setup instead of "
     "each getting its own. That means fewer setups to prepare while playing. The first start "
     "with this on prepares the setups again once, so expect a slow first start.",
     "In our tests this was slightly slower. May help some games."},
    {"KYTY_TABLE_MODE", "Shaders read game data directly (experimental, not recommended)",
     "Shaders that only read game data get it straight from memory addresses the emulator hands "
     "them for each drawing step, instead of through per-step binding tables. Meant to save setup "
     "work per drawing step. The first start with this on prepares the shaders again once, so "
     "expect a slow first start.",
     "In our tests no clear difference. Experimental, may help some games."},
    {"KYTY_DEBUG_DRAW_REPEAT", "Count repeating draws (diagnostics)",
     "Every 120 frames the console window shows how many drawing steps look exactly like one "
     "from the last frames. That tells whether reusing them would help in this game.",
     "Diagnostics only, no speed gain."},
    {"KYTY_PROTECT_EXEC_QUERY", "Old memory protection check (not recommended)",
     "Brings back the old way of checking whether game code may run from a piece of memory: the "
     "emulator asks Windows each time instead of remembering it. Only there to compare against "
     "the new way if a game misbehaves.",
     "For comparison only, usually much slower."},
    {"KYTY_STRICT_TEXTURES", "Stop at the first texture that cannot be shown (testing)",
     "Normally a texture the emulator cannot show yet is replaced by a plain stand-in and a red " 
     "line names it in the console window, so the game keeps running. With this on, the emulator " 
     "stops at the first such texture a shader really uses, so it can be found and fixed.",
     "For testing only: the game ends where a texture is missing."},
}};

// Whether the switch is on by default for this title.
[[nodiscard]] inline bool IsSwitchDefaultOn(const EmulatorSwitch& entry, const QString& title_id) {
	return entry.default_titles != nullptr && !title_id.trimmed().isEmpty() &&
	       QString::fromLatin1(entry.default_titles)
	           .split(QLatin1Char(','))
	           .contains(title_id.trimmed(), Qt::CaseInsensitive);
}

// The profile's state of a switch: "NAME" in the stored list is on, "-NAME" is off (it overrides
// a title default), neither is the title default.
[[nodiscard]] inline bool IsSwitchOn(const EmulatorSwitch& entry, const QStringList& stored,
                                     const QString& title_id) {
	const auto name = QString::fromLatin1(entry.name);
	if (stored.contains(name)) {
		return true;
	}
	if (stored.contains(QLatin1Char('-') + name)) {
		return false;
	}
	return IsSwitchDefaultOn(entry, title_id);
}

#endif /* LAUNCHER_INCLUDE_EMULATORSWITCHES_H_ */
