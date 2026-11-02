#include "app/app.h"
#include "core/input.h"

namespace street {
void ScriptPad(int port, input::Pad& pad);  // pad_script.cpp
}

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"The Godfather",               // window and splash
    .short_title = L"The Godfather",              // message boxes
    .name = L"GODFATHER",                      // window classes, crash dumps, handover event
    .log = "godfather",                        // log file prefix
    .settings = "godfather-settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put your own game files in the assets folder: its disc image (.iso), or default.xbe with the extracted files beside it.",
    .images = {{"default.xbe", "GODFATHER"}},  // each recompiled xbe and its exe, for title launches
    .options = {},                        // game settings kept in the settings file
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = nullptr,                      // after the settings load, before the window and boot
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = nullptr,                   // window messages first; true takes one
    .ui = nullptr,                        // ImGui drawing each frame
    .pad = street::ScriptPad,              // scripted test presses (pad_script.cpp), none unless asked for
    .rumble = nullptr,                    // changes a rumble before it is set
};
