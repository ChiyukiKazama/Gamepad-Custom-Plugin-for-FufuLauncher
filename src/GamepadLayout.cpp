#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "MinHook.h"
#include "GamepadLayout.h"
#include "GameBuild.h"
#include "NativePages.h"

namespace {
    // CN/OS 7.2 UI manager reference-resolution getter. The normal gamepad
    // branch returns 1920x1080 and the PC branch returns 1600x900.
    constexpr const char* ReferenceSignature =
        "56 48 83 EC 20 48 89 CA 80 3D ? ? ? ? 00 75 ? "
        "48 BE 00 00 A0 44 00 00 34 44 8B 82 ? ? ? ? 83 F8 02 74 ? "
        "83 F8 01 75 ? 48 BE 00 00 C8 44 00 00 61 44 EB ? "
        "80 3D ? ? ? ? 00 48 8B 05 ? ? ? ? 75 ? 48 8B 80 ? ? ? ? "
        "48 85 C0 74 ? 8B 88 ? ? ? ? E8 ? ? ? ? 84 C0 75 ? "
        "48 BE 00 00 F0 44 00 00 87 44 48 89 F0";
    constexpr int GamepadUiMode = 2;

    // The native eight-byte Vector2 is passed in RAX on this build.
    constexpr std::uint64_t GamepadReference = 0x4487000044F00000;
    constexpr std::uint64_t KeyboardReference = 0x4461000044C80000;
    using GetReference = std::uint64_t (WINAPI*)(void*, void*);
    GetReference g_OriginalReference = nullptr;
    GamepadLayout::Logger g_Log = nullptr;
    unsigned g_UiModeOffset = 0;
    std::atomic<bool> g_LoggedScale{ false };

    std::uint64_t WINAPI HookReference(void* ui, void* method) {
        const auto native = g_OriginalReference(ui, method);
        if (native != GamepadReference || !ui ||
            *reinterpret_cast<const int*>(static_cast<const unsigned char*>(ui) + g_UiModeOffset) != GamepadUiMode)
            return native;

        if (!g_LoggedScale.exchange(true, std::memory_order_relaxed))
            g_Log("Gamepad UI reference changed from 1920x1080 to 1600x900");
        return KeyboardReference;
    }
}

void GamepadLayout::Install(Logger log, const GameBuild::Profile& build) {
    auto* entry = static_cast<unsigned char*>(NativePages::FindNative(ReferenceSignature));
    if (!entry) {
        log("Gamepad UI scale signature absent or ambiguous; native layout retained");
        return;
    }
    unsigned modeOffset = 0, globalModeOffset = 0;
    std::memcpy(&modeOffset, entry + 0x1D, sizeof(modeOffset));
    std::memcpy(&globalModeOffset, entry + 0x55, sizeof(globalModeOffset));
    if (modeOffset != build.uiModeOffset || globalModeOffset != modeOffset) {
        log("Gamepad UI mode field mismatched; native layout retained");
        return;
    }
    g_Log = log;
    g_UiModeOffset = modeOffset;
    const auto created = MH_CreateHook(entry, reinterpret_cast<void*>(&HookReference),
        reinterpret_cast<void**>(&g_OriginalReference));
    if (created != MH_OK) {
        log("Gamepad UI scale hook creation failed; native layout retained");
        return;
    }
    if (MH_EnableHook(entry) != MH_OK) {
        MH_RemoveHook(entry);
        log("Gamepad UI scale hook activation failed; native layout retained");
        return;
    }
    log("Gamepad UI scale hook installed");
}
