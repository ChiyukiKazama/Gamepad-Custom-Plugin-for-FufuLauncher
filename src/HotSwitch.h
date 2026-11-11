#pragma once
#include <Windows.h>
#include <Xinput.h>

namespace GameBuild { struct Profile; }
namespace HotSwitch {
    using Logger = void (*)(const char*);
    using Resolver = void* (*)(const char*);
    void Configure(Logger log, bool enabled, bool diagnostics);
    void Prepare(Resolver resolve, const GameBuild::Profile& build); // Startup-only, unique native matches required.
    void Install(); // Worker, after main-plugin hooks finish initialization.
    bool IsEnabled();
    bool IsReady();
    void ObserveXInput(unsigned index, const XINPUT_GAMEPAD& pad); // Existing input mutex held.
    void NotifyGamepadActivity(); // Other verified native gamepad adapters.
    void DisconnectXInput(unsigned index);
}
