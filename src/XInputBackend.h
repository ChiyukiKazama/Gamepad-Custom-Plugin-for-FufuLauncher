#pragma once
#include <Windows.h>
#include <Xinput.h>
#include <cstddef>

namespace XInputBackend {
    using Logger = void (*)(const char*);
    using Wanted = bool (*)();
    using Observed = void (*)();
    using Process = void (*)(unsigned gamepadIndex, XINPUT_GAMEPAD& pad);
    using Disconnect = void (*)(unsigned gamepadIndex);
    using Activity = void (*)(unsigned gamepadIndex, const XINPUT_GAMEPAD* pad);
    void Configure(Logger log, Wanted wanted, Observed observed, Process process,
        Disconnect disconnect, Activity activity);
    bool MatchesModule(const wchar_t* name, std::size_t length);
    void Install(); // Worker only; inspects already-loaded XInput DLLs.
    void PollActivity(); // Worker only; no game-input routing or extra DLL loads.
}
