#include "app/app.h"

namespace dev {
void Pad(int port, input::Pad& p);  // dev_input.cpp
}
namespace game {
void Loaded();  // game/hooks.cpp
}

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Juiced",              // window and splash
    .short_title = L"Juiced",        // message boxes
    .name = L"Juiced",               // window classes, crash dumps, handover event
    .log = "juiced",                 // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "Juiced"}},  // each recompiled xbe and its exe, for title launches
    .options = {},                        // game settings kept in the settings file
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = nullptr,                      // after the settings load, before the window and boot
    .loaded = game::Loaded,               // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = nullptr,                   // window messages first; true takes one
    .ui = nullptr,                        // ImGui drawing each frame
    .pad = dev::Pad,                      // changes a polled pad: JUICED_DEV's scripted presses
    .rumble = nullptr,                    // changes a rumble before it is set
};
