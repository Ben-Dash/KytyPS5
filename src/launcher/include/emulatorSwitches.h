#ifndef LAUNCHER_INCLUDE_EMULATORSWITCHES_H_
#define LAUNCHER_INCLUDE_EMULATORSWITCHES_H_

#include <QString>
#include <QStringList>

#include <array>
#include <string_view>

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
	const char* default_titles = nullptr; // Title ids (comma separated) where it is on by default, "*" = all
};

inline constexpr std::array<EmulatorSwitch, 29> EMULATOR_SWITCHES = {{
    {"KYTY_COMPUTE_CHAIN", "Chain compute work without extra waits",
     "Compute steps that follow each other run without a full wait between them, as long as the "
     "game itself does not ask to wait in between. On by default for ASTRO BOT, ASTRO's PLAYROOM "
     "and Ghost of Yotei; uncheck it to turn it off for that game.",
     "In our tests clearly faster in Ghost of Yotei and the ASTRO games. If a game shows new "
     "graphics errors, try it off.",
     "PPSA21564,PPSA01325,PPSA26344"},
    {"KYTY_OCCLUSION_QUERIES", "Real visibility tests (experimental)",
     "Games ask the graphics card whether an object would be visible at all before they draw it "
     "in full. The emulator normally answers \"visible\" every time, so hidden things are drawn "
     "too and lens flares or glows can shine through walls. With this on, the graphics card "
     "really counts what is visible.",
     "Not tested in games yet. May save work in games that hide a lot, may show new graphics "
     "errors."},
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
    {"KYTY_VTX_EXPLODE_CHECK", "Report exploding triangles (diagnostics)",
     "Every drawn vertex is checked for a broken screen position (not a number, divided by zero, "
     "or far outside the screen), and each drawing step's largest index is compared with its "
     "vertex data. Each finding prints one red line in the console window, naming the shader and "
     "the buffers involved. Nothing in the picture changes. Uses its own shader cache, so the "
     "first start with this on prepares the shaders again.",
     "Diagnostics only, costs speed."},
    {"KYTY_BINDLESS_EAGER", "Load texture tables of rare compute steps right away",
     "Some compute steps read their textures from a list the game rebuilds at a new place each "
     "time and run only now and then. Without this the emulator sets up the textures of such a "
     "list only after the step asked for them, which is too late, so the step sees empty "
     "textures (Ghost of Yotei: the moonlight stays dark). With this on, a new list is set up "
     "completely the first time a compute step uses it (3D textures only). On by default; uncheck it to turn it off.",
     "Under test: meant to fix the empty moon volumes in Ghost of Yotei. May cost a short "
     "hitch when such a list is first seen.", "*"},
    {"KYTY_DISPATCH_SNAPSHOT", "Record compute steps for offline replay (diagnostics)",
     "Writes compute steps of the game to the _snapshots folder: the shader and every input it "
     "reads, and what it wrote. kyty_emulator --dispatch-replay runs them again later without the "
     "game, to test a fix in seconds. Which steps: the shader hashes or frame:<n> in "
     "dispatch_snapshot.txt next to the emulator, otherwise every step of frame 600. Each "
     "recorded step waits for the graphics card, so the game stutters while it records.",
     "Diagnostics only, costs speed and disk space while recording."},
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
    {"KYTY_BINDLESS_TSHARP_CHECK", "Check texture table entries before use (testing)",
     "Games keep textures in tables the emulator reads while the game runs. Some entries hold "
     "other data that only looks like a texture. With this on, every entry is checked first; one "
     "that cannot be a texture gets the plain stand-in and a red line instead of reaching the "
     "texture code.",
     "New, under test in Ghost of Yotei. Should only change the console lines."},
    {"KYTY_BINDLESS_TYPED", "Read each table texture by its own kind (testing)",
     "A shader reads textures from a table by number. Today the emulator decides once per shader "
     "which kind of texture (flat, volume, whole numbers) the table holds; every texture of "
     "another kind shows a plain stand-in. With this on, each texture is read as the kind it is, "
     "as on the console.",
     "New, under test in Ghost of Yotei. Shaders are translated anew once."},
    {"KYTY_BINDLESS_KEY_BOUNDS", "Ignore stale texture-table requests",
     "Texture tables only load the entries a shader can really reach, and requests left over from an older table are dropped. On by default; uncheck to turn it off.",
     "Fixed a GPU crash at the Torii in Ghost of Yotei (4 of 4 runs stable).", "*"},
    {"KYTY_COLOR_FEEDBACK_COPY", "Copy a picture a draw reads and paints at once (testing)",
     "When a draw reads the same picture it paints into (decals on walls), it reads a copy taken just before instead. Costs a copy per such draw.",
     "New, under test in Ghost of Yotei (smeared interior walls)."},
    {"KYTY_COLOR_FEEDBACK_LAYOUT", "Read-while-painting mode for pictures (testing)",
     "A picture a draw reads and paints at the same time is set up in the graphics driver's special read-while-painting mode instead of a general one.",
     "New, under test in Ghost of Yotei (smeared interior walls)."},
    {"KYTY_META_CLEAR_ZMASK", "Check depth-clear fills before clearing (testing)",
     "A fill over depth-buffer bookkeeping memory only counts as a depth clear when its values really mean cleared.",
     "New, under test in Ghost of Yotei (depth wiped every frame for two targets)."},
    {"KYTY_BC_BUFFER_WRITES", "Compressed textures take GPU buffer writes (testing)",
     "A compressed texture the game fills piece by piece through a buffer gets those pieces; before only writes starting at its first byte counted.",
     "New, under test in Ghost of Yotei (empty reflection cubes at the Torii)."},
    {"KYTY_BINDLESS_REFRESH", "Refresh table textures the game rewrites (testing)",
     "A texture from a texture table is checked once per frame for new contents written by the game and uploaded again when it changed.",
     "New, under test in Ghost of Yotei."},
    {"KYTY_WAVE_BRANCH_FULL", "Shader branches over the whole wave (testing)",
     "Shader branches decide over all 64 lanes of a wave, including switched-off lanes, as an upstream change does. Shaders are translated anew.",
     "New, under test in Ghost of Yotei cutscenes."},
    {"KYTY_PIXEL_PROVOKING_VERTEX", "Flat pixel inputs from the provoking vertex (testing)",
     "Flat values in pixel shaders come from the vertex the game marks as provoking, not always the first.",
     "Tested in Ghost of Yotei: no visible difference."},
    {"KYTY_DEBUG_FEEDBACK_LOOP", "Log pictures read and painted in one draw (diagnostics)",
     "Prints a red line for each picture a draw reads and paints at the same time.",
     "Diagnostics only."},
    {"KYTY_DEBUG_META_CLEAR", "Log depth-buffer clears (diagnostics)",
     "Prints a red line for each recorded depth-buffer clear and each draw that clears a depth buffer because of one.",
     "Diagnostics only."},
    {"KYTY_DEBUG_DEPTHONLY", "Log depth-only draws (diagnostics)",
     "Prints a line for each kind of draw that only writes depth: whether the game set a pixel shader, its kill and export settings.",
     "Diagnostics only."},
    {"KYTY_PIXEL_HISTORY", "Log who wrote one pixel (diagnostics)",
     "After every draw and compute step the emulator reads one pixel back and prints a red line whenever it changed: frame, step, shader hashes, value before and after. Which pixel: x,y[,0xaddress] in line 1 of pixel_history.txt next to the emulator, optional frame range first-last in line 2; otherwise the centre of the picture. Costs a lot of speed.",
     "Diagnostics only."},
    {"KYTY_DEBUG_VK_ERRORS_NONFATAL", "Keep running on Vulkan validation errors (diagnostics)",
     "With the Vulkan validation layer on, an error no longer stops the emulator: it is printed as a red line, each kind three times and then every 1000th time.",
     "Diagnostics only, for sync validation runs."},
    {"KYTY_SAMPLER_COMPARE_SPLIT", "Separate depth-compare samplers (testing)",
     "When a shader uses one sampler both for a normal texture and for a shadow (depth-compare) texture, the normal texture gets its own sampler without the compare. Before, it was compared too, which the graphics driver does not define. Shaders are translated anew.",
     "New, under test in Ghost of Yotei (Vulkan validation error on a colour texture; suspected black spots)."},
    {"KYTY_CONSOLE_COLORS", "Colored console window",
     "The console window next to the game shows its lines in colors by kind: errors red, fixed " 
     "things green, warnings yellow, audio blue, shaders purple, memory turquoise. Uncheck it for " 
     "the plain console.",
     "Only changes how the console looks, no effect on the game.", "*"},
}};

// Whether the switch is on by default for this title.
[[nodiscard]] inline bool IsSwitchDefaultOn(const EmulatorSwitch& entry, const QString& title_id) {
	if (entry.default_titles != nullptr && std::string_view(entry.default_titles) == "*") {
		return true;
	}
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
