#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Xinput.h>
#include <winternl.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "MinHook.h"
#include "ButtonRouter.h"
#include "NativePages.h"
#include "GamepadButton.h"
#include "Sensitivity.h"
#include "SensitivityFactor.h"
#include "LiveConfig.h"
#include "XInputBackend.h"
#include "DualSenseBackend.h"
#include "DualSenseHidBackend.h"
#include "DualSenseIdentity.h"
#include "DualSenseState.h"

namespace {
    HMODULE g_Module = nullptr;
    HANDLE g_ModuleEvent = nullptr;
    std::filesystem::path g_Directory;
    bool g_Diagnostics = false;
    bool g_EdgeView = true;
    std::atomic<bool> g_MapEnabled{ false };
    GamepadButton g_MapButton = GamepadButton::Parse(L"View");
    bool g_LogEnabled = false;
    std::mutex g_LogMutex;
    std::mutex g_InputMutex;
    ButtonRouter g_XInputRouter;
    struct DualSenseController {
        ButtonRouter router;
        DualSenseController() { router.Rebind(); }
    };
    std::unordered_map<void*, DualSenseController> g_DualSenseControllers; // Existing input mutex held.
    std::unordered_map<void*, DualSenseController> g_DualSenseHidControllers;
    static_assert(GamepadButton::TouchpadMask == DualSenseTouchpadMask);

    void Log(const char* message) {
        if (!g_LogEnabled) return;
        std::lock_guard<std::mutex> lock(g_LogMutex);
        std::ofstream file(g_Directory / L"GamepadCustomPlugin.log", std::ios::app);
        file << GetTickCount64() << " " << message << '\n';
    }

    bool RequestMap(void* context) {
        Log(context ? "PS5 map button pressed; carrier=Touchpad" : "Xbox map button pressed; carrier=View");
        return NativePages::RequestMap(g_MapEnabled.load(std::memory_order_relaxed));
    }
    void InputObserved() {
        if (NativePages::RequestActivation()) SetEvent(g_ModuleEvent);
    }
    bool InputWanted() {
        return g_MapEnabled.load(std::memory_order_relaxed) || g_Diagnostics;
    }
    bool ProcessDualSense(void* device, XINPUT_GAMEPAD& pad) {
        std::lock_guard<std::mutex> lock(g_InputMutex);
        auto& controller = g_DualSenseControllers.try_emplace(device).first->second;
        const bool enabled = g_MapEnabled.load(std::memory_order_relaxed);
        const bool captured = controller.router.Sample(0, g_MapButton.IsDown(pad),
            (enabled || g_Diagnostics) && (!enabled || g_MapButton.CanStart(pad, DualSenseTouchpadMask)),
            RequestMap, &controller);
        if (captured) g_MapButton.EmitCarrier(pad, DualSenseTouchpadMask);
        return captured;
    }
    void DisconnectDualSense(void* device) {
        std::lock_guard<std::mutex> lock(g_InputMutex);
        g_DualSenseControllers.erase(device);
    }
    bool ProcessDualSenseHid(void* device, XINPUT_GAMEPAD& pad) {
        std::lock_guard<std::mutex> lock(g_InputMutex);
        auto& controller = g_DualSenseHidControllers.try_emplace(device).first->second;
        const bool enabled = g_MapEnabled.load(std::memory_order_relaxed);
        const bool captured = controller.router.Sample(0, g_MapButton.IsDown(pad),
            (enabled || g_Diagnostics) && (!enabled || g_MapButton.CanStart(pad, DualSenseTouchpadMask)),
            RequestMap, &controller);
        if (captured) g_MapButton.EmitCarrier(pad, DualSenseTouchpadMask);
        return captured;
    }
    void DisconnectDualSenseHid(void* device) {
        std::lock_guard<std::mutex> lock(g_InputMutex);
        g_DualSenseHidControllers.erase(device);
    }

    bool ApplyLiveSettings(const LiveSettings& next, LiveSettings& current) {
        const bool rebind = next.mapEnabled != current.mapEnabled || !next.mapButton.SameBinding(current.mapButton);
        const bool factorChanged = next.sensitivity != current.sensitivity;
        if (rebind) {
            std::lock_guard<std::mutex> lock(g_InputMutex);
            g_MapEnabled.store(next.mapEnabled && next.mapButton.IsValid(), std::memory_order_relaxed);
            g_MapButton = next.mapButton;
            g_XInputRouter.Rebind();
            for (auto& controller : g_DualSenseControllers) controller.second.router.Rebind();
            for (auto& controller : g_DualSenseHidControllers) controller.second.router.Rebind();
            NativePages::CancelMapRequest();
        }
        if (factorChanged) Sensitivity::SetFactor(next.sensitivity);
        current = next;
        return rebind || factorChanged;
    }

    void ProcessXInput(unsigned controller, XINPUT_GAMEPAD& pad) {
        std::lock_guard<std::mutex> lock(g_InputMutex);
        const bool enabled = g_MapEnabled.load(std::memory_order_relaxed);
        const bool down = !g_MapButton.IsNativeOnly() && g_MapButton.IsDown(pad);
        const bool canStart = !enabled || g_MapButton.CanStart(pad);
        const bool captured = g_XInputRouter.Sample(controller, down,
            (enabled || g_Diagnostics) && canStart, RequestMap, nullptr);
        if (captured) g_MapButton.EmitCarrier(pad);
    }
    void DisconnectXInput(unsigned controller) {
        std::lock_guard<std::mutex> lock(g_InputMutex);
        g_XInputRouter.Disconnect(controller);
    }

    void InstallAvailableBackends() {
        NativePages::Prepare(); NativePages::Install();
        XInputBackend::Install();
        DualSenseHidBackend::Install();
        // Sony identity view. Installed after the HID adapter so that adapter
        // keeps the real hid.dll exports it captured.
        DualSenseIdentity::Install();
    }

    // Loader notifications run under the loader lock. Only signal here;
    // resolve exports/install hooks on our worker after the callback returns.
    struct NotificationEntry {
        ULONG flags;
        const UNICODE_STRING* fullName;
        const UNICODE_STRING* baseName;
        PVOID base;
        ULONG imageSize;
    };
    union NotificationData { NotificationEntry loaded; NotificationEntry unloaded; };
    using NotifyCallback = void (CALLBACK*)(ULONG, const NotificationData*, PVOID);
    using RegisterNotification = NTSTATUS (NTAPI*)(ULONG, NotifyCallback, PVOID, PVOID*);

    void CALLBACK OnModuleLoaded(ULONG reason, const NotificationData* data, PVOID) {
        if (reason != 1 || !data) return;
        const auto name = data->loaded.baseName;
        const auto full = data->loaded.fullName;
        if ((full && NativePages::MatchesMainPath(full->Buffer, full->Length / sizeof(wchar_t))) ||
            (name && (XInputBackend::MatchesModule(name->Buffer, name->Length / sizeof(wchar_t)) ||
                (name->Buffer && SameName(std::wstring_view(name->Buffer, name->Length / sizeof(wchar_t)), L"hid.dll"))))) {
            SetEvent(g_ModuleEvent);
        }
    }

    DWORD WINAPI Worker(void*) {
        wchar_t path[32768]{};
        if (!GetModuleFileNameW(g_Module, path, ARRAYSIZE(path))) return 0;
        g_Directory = std::filesystem::path(path).parent_path();
        const auto config = g_Directory / L"config.ini";
        g_LogEnabled = GetPrivateProfileIntW(L"DebugLog", L"Value", 0, config.c_str()) != 0;
        g_Diagnostics = GetPrivateProfileIntW(L"InputDiagnostics", L"Value", 0, config.c_str()) != 0;
        g_EdgeView = GetPrivateProfileIntW(L"DualSenseEdgeView", L"Value", 1, config.c_str()) != 0;
        ConfigWatcher watcher;
        const bool watching = watcher.Start(g_Directory);
        LiveSettings current, initial;
        if (!ReadLiveSettings(config, initial, false))
            Log("Initial config unreadable/invalid; live features remain disabled until a valid save");
        Sensitivity::Configure(Log, initial.sensitivity, g_Diagnostics);
        ApplyLiveSettings(initial, current);
        XInputBackend::Configure(Log, InputWanted, InputObserved, ProcessXInput,
            DisconnectXInput);
        DualSenseBackend::Configure(Log, InputWanted, InputObserved, ProcessDualSense, DisconnectDualSense);
        DualSenseHidBackend::Configure(Log, InputWanted, InputObserved, ProcessDualSenseHid, DisconnectDualSenseHid);
        DualSenseIdentity::Configure(Log, g_EdgeView);
        // Keep the verified map route available for later live enabling. Resolve
        // it once; changing config never repeats native map signature searches.
        NativePages::Configure(Log, true, g_Diagnostics, true, g_Directory.c_str());
        GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        const auto executable = std::filesystem::path(path).filename().wstring();
        if (_wcsicmp(executable.c_str(), L"YuanShen.exe") &&
            _wcsicmp(executable.c_str(), L"GenshinImpact.exe")) {
            Log("Unsupported process; plugin left inactive");
            return 0;
        }
        Log("GamepadCustomPlugin 1.1.0 started; map binding and camera sensitivity; diagnostics opt-in");
        if (!watching) Log("Config notifications unavailable; settings require game restart (no polling fallback)");
        if (MH_Initialize() != MH_OK) {
            Log("MinHook initialization failed; input unchanged");
            return 0;
        }
        g_ModuleEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_ModuleEvent) return 0;
        auto registerNotification = reinterpret_cast<RegisterNotification>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
        PVOID cookie = nullptr;
        const bool registered = registerNotification &&
            registerNotification(0, OnModuleLoaded, nullptr, &cookie) >= 0;
        InstallAvailableBackends();
        if (!registered) {
            Log("DLL notification unavailable; only already-loaded backends were inspected");
        }
        HANDLE events[] = {g_ModuleEvent, watcher.Event()};
        ULONGLONG reloadAfter = 0;
        // Block while idle. Only a config event starts a 100 ms save-settling
        // delay; input processing runs only when the game reads its controllers.
        for (;;) {
            DWORD timeout = INFINITE;
            if (reloadAfter) {
                const auto now = GetTickCount64();
                timeout = (std::min)(timeout, static_cast<DWORD>(now < reloadAfter ? reloadAfter - now : 0));
            }
            const DWORD result = WaitForMultipleObjects(watching ? 2 : 1, events, FALSE, timeout);
            if (result == WAIT_OBJECT_0) InstallAvailableBackends();
            else if (watching && result == WAIT_OBJECT_0 + 1) {
                if (watcher.Consume()) reloadAfter = GetTickCount64() + 100;
            }
            else if (result != WAIT_TIMEOUT) break;
            if (reloadAfter && GetTickCount64() >= reloadAfter) {
                reloadAfter = 0;
                LiveSettings next;
                if (!ReadLiveSettings(config, next)) {
                    Log("Config save unreadable/incomplete/invalid; keeping last valid live settings");
                } else if (ApplyLiveSettings(next, current)) {
                    InstallAvailableBackends();
                    Log("Map switch/button and sensitivity reloaded; startup-only options unchanged");
                }
            }
        }
        return 0;
    }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_Module = module;
        DisableThreadLibraryCalls(module);
        // A game plugin remains loaded until process exit; live FreeLibrary is unsupported.
        if (HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
