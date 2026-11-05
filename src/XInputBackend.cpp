#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Xinput.h>
#include <array>
#include <atomic>
#include <cwchar>
#include "MinHook.h"
#include "XInputBackend.h"

namespace {
    using GetState = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
    struct Backend {
        const wchar_t* name;
        void* target = nullptr;
        GetState original = nullptr;
        std::atomic<bool> observed{ false };
    };
    std::array<Backend, 3> g_Backends{{
        {L"xinput1_3.dll"}, {L"xinput9_1_0.dll"}, {L"xinput1_4.dll"}
    }};
    XInputBackend::Logger g_Log = nullptr;
    XInputBackend::Wanted g_Wanted = nullptr;
    XInputBackend::Observed g_Observed = nullptr;
    XInputBackend::Process g_Process = nullptr;
    XInputBackend::Disconnect g_Disconnect = nullptr;
    XInputBackend::Activity g_Activity = nullptr;
    thread_local unsigned g_InputDepth = 0;

    void Log(const char* text) { if (g_Log) g_Log(text); }

    template<std::size_t Index>
    DWORD WINAPI HookGetState(DWORD controller, XINPUT_STATE* state) {
        auto& backend = g_Backends[Index];
        ++g_InputDepth;
        const DWORD result = backend.original(controller, state);
        const bool outermost = --g_InputDepth == 0;
        if (!outermost || controller >= 4) return result;
        if (g_Observed) g_Observed();
        if (!backend.observed.exchange(true, std::memory_order_relaxed)) {
            Log(Index == 0 ? "Game reads XInput 1.3" :
                Index == 1 ? "Game reads XInput 9.1.0" : "Game reads XInput 1.4");
        }
        if (!g_Wanted || !g_Wanted()) return result;
        if (result != ERROR_SUCCESS) {
            if (g_Disconnect) g_Disconnect(controller);
        } else if (state && g_Process) {
            g_Process(controller, state->Gamepad);
        }
        return result;
    }

    const std::array<void*, 3> g_Detours{{
        reinterpret_cast<void*>(&HookGetState<0>),
        reinterpret_cast<void*>(&HookGetState<1>),
        reinterpret_cast<void*>(&HookGetState<2>)
    }};
}

void XInputBackend::Configure(Logger log, Wanted wanted, Observed observed, Process process,
    Disconnect disconnect, Activity activity) {
    g_Log = log; g_Wanted = wanted; g_Observed = observed; g_Process = process;
    g_Disconnect = disconnect; g_Activity = activity;
}

bool XInputBackend::MatchesModule(const wchar_t* name, std::size_t length) {
    if (!name) return false;
    for (const auto& backend : g_Backends) {
        if (length == std::wcslen(backend.name) && !_wcsnicmp(name, backend.name, length)) return true;
    }
    return false;
}

void XInputBackend::Install() {
    for (std::size_t index = 0; index < g_Backends.size(); ++index) {
        auto& backend = g_Backends[index];
        if (backend.target) continue;
        const HMODULE module = GetModuleHandleW(backend.name);
        if (!module) continue; // Never loads extra input DLLs into the game.
        void* target = reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState"));
        if (!target) continue;
        bool duplicate = false;
        for (const auto& other : g_Backends) {
            if (other.target == target) duplicate = true;
        }
        if (duplicate) {
            backend.target = target;
            continue;
        }
        const auto created = MH_CreateHook(target, g_Detours[index],
            reinterpret_cast<void**>(&backend.original));
        if (created != MH_OK) {
            Log("XInputGetState hook creation failed; backend unchanged");
            continue;
        }
        if (MH_EnableHook(target) != MH_OK) {
            MH_RemoveHook(target);
            Log("XInputGetState hook activation failed; backend unchanged");
            continue;
        }
        backend.target = target;
        Log("XInputGetState hook installed");
    }
}

void XInputBackend::PollActivity() {
    // Keyboard mode may stop game-driven controller reads. Reuse one installed backend.
    if (!g_Activity) return;
    GetState read = nullptr;
    for (const auto& backend : g_Backends) {
        if (backend.original) { read = backend.original; break; }
    }
    if (!read) return;
    for (unsigned controller = 0; controller < 4; ++controller) {
        XINPUT_STATE state{};
        ++g_InputDepth; // Polling must not be mistaken for game input.
        const DWORD result = read(controller, &state);
        --g_InputDepth;
        g_Activity(controller, result == ERROR_SUCCESS ? &state.Gamepad : nullptr);
    }
}
