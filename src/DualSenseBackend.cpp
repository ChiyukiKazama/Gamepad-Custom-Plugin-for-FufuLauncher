#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include "MinHook.h"
#include "DualSenseBackend.h"
#include "DualSenseState.h"

namespace {
    // Rewired_Windows.dll layout from the CN 7.2 dump and parser bodies.
    namespace Layout {
        constexpr std::size_t Buttons = 0x48, Axes = 0x30, Hats = 0x20;
        constexpr std::size_t Disposed = 0x50, Report = 0xC0, Bluetooth = 0x163;
        constexpr std::size_t ButtonValue = 0x20, ButtonTime = 0x28;
        constexpr std::size_t AxisValue = 0x20, AxisTime = 0x40;
        constexpr std::size_t HatValue = 0x3C, HatTime = 0x30;
    }
    constexpr const char* ParseSignature =
        "41 57 41 56 56 57 53 48 83 EC 40 0F 29 74 24 30 66 0F 28 F3 "
        "44 89 C7 48 89 D3 49 89 CF 80 3D ? ? ? ? 00 0F 84 ? ? ? ? "
        "48 85 DB 74 ? 49 8B 8F C0 00 00 00 48 85 C9 0F 84 ? ? ? ? "
        "44 8B 49 14 41 39 F9 7E ? 31 C0 E9 ? ? ? ? 66 0F 28 C6 "
        "F2 41 0F 5C 87 C8 00 00 00 F2 0F 5A C0 F3 41 0F 11 87 48 01 00 00 "
        "F2 41 0F 11 B7 C8 00 00 00 C7 44 24 28 00 00 00 00 C7 44 24 20 00 00 00 00 "
        "48 89 DA 41 89 F8 E8 ? ? ? ?";
    constexpr const char* DisposeSignature =
        "55 56 57 53 48 83 EC 48 48 8D 6C 24 40 48 C7 45 00 FE FF FF FF "
        "80 79 50 00 0F 85 ? ? ? ? 48 89 CE C6 41 50 01 84 D2 0F 84 ? ? ? ? "
        "48 8B 46 10 48 85 C0 74 ? 8B 78 18 85 FF 7E ? 31 DB EB ? "
        "66 66 66 66 66 2E 0F 1F 84 00 00 00 00 00 FF C3 39 DF 74 ? "
        "48 8B 46 10 48 85 C0 0F 84 ? ? ? ? 3B 58 18 0F 83 ? ? ? ? "
        "48 63 CB 48 8B 44 C8 20 48 85 C0 0F 84 ? ? ? ? C7 40 18 00 00 00 00 "
        "48 8B 40 10 48 85 C0 74 ? 48 8B 50 28 48 8B 48 40 FF 50 18 EB ? "
        "80 BE 45 01 00 00 00";
    using Parse = bool (WINAPI*)(void*, const void*, int, double, void*);
    using Dispose = void (WINAPI*)(void*, bool, void*);
    Parse g_OriginalParse = nullptr;
    Dispose g_OriginalDispose = nullptr;
    void* g_ParseEntry = nullptr;
    void* g_DisposeEntry = nullptr;
    DualSenseBackend::Logger g_Log = nullptr;
    DualSenseBackend::Wanted g_Wanted = nullptr;
    DualSenseBackend::Observed g_Observed = nullptr;
    DualSenseBackend::Process g_Process = nullptr;
    DualSenseBackend::Disconnect g_Disconnect = nullptr;
    bool g_Prepared = false, g_InstallAttempted = false;
    std::atomic<unsigned> g_Traced{ 0 };
    std::mutex g_StateMutex;
    std::unordered_map<void*, unsigned> g_Disposing;

    void Log(const char* text) { if (g_Log) g_Log(text); }
    template<class T> T& Field(void* object, std::size_t offset) {
        return *reinterpret_cast<T*>(static_cast<unsigned char*>(object) + offset);
    }
    bool ReadState(void* driver, DualSenseState& state, bool& bluetooth) {
        __try {
            if (!driver || Field<unsigned char>(driver, Layout::Disposed)) return false;
            const auto buttons = Field<void*>(driver, Layout::Buttons);
            const auto axes = Field<void*>(driver, Layout::Axes);
            const auto hats = Field<void*>(driver, Layout::Hats);
            const auto report = Field<void*>(driver, Layout::Report);
            if (!buttons || !axes || !hats || !report ||
                Field<std::size_t>(buttons, 0x18) != state.buttons.size() ||
                Field<std::size_t>(axes, 0x18) != state.axes.size() ||
                Field<std::size_t>(hats, 0x18) != 1 || Field<int>(report, 0x14) != 64) return false;
            const auto bytes = Field<const unsigned char*>(report, 0x18);
            bluetooth = Field<unsigned char>(driver, Layout::Bluetooth) != 0;
            if (!bytes || bytes[0] != (bluetooth ? 0x31 : 0x01)) return false;
            for (std::size_t index = 0; index < state.buttons.size(); ++index) {
                const auto element = Field<void*>(buttons, 0x20 + index * sizeof(void*));
                if (!element || Field<unsigned char>(element, Layout::ButtonValue) > 1) return false;
                state.buttons[index] = Field<unsigned char>(element, Layout::ButtonValue) != 0;
            }
            for (std::size_t index = 0; index < state.axes.size(); ++index) {
                const auto element = Field<void*>(axes, 0x20 + index * sizeof(void*));
                if (!element) return false;
                state.axes[index] = Field<int>(element, Layout::AxisValue);
            }
            const auto hat = Field<void*>(hats, 0x20);
            if (!hat) return false;
            state.hat = Field<int>(hat, Layout::HatValue);
            return state.IsValid();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool WriteChanges(void* driver, const DualSenseState& before, const DualSenseState& after, double time) {
        __try {
            const auto buttons = Field<void*>(driver, Layout::Buttons);
            const auto axes = Field<void*>(driver, Layout::Axes);
            for (std::size_t index = 0; index < after.buttons.size(); ++index) {
                if (before.buttons[index] == after.buttons[index]) continue;
                const auto element = Field<void*>(buttons, 0x20 + index * sizeof(void*));
                Field<unsigned char>(element, Layout::ButtonValue) = after.buttons[index] ? 1 : 0;
                Field<double>(element, Layout::ButtonTime) = time;
            }
            for (std::size_t index = 4; index < after.axes.size(); ++index) {
                if (before.axes[index] == after.axes[index]) continue;
                const auto element = Field<void*>(axes, 0x20 + index * sizeof(void*));
                Field<int>(element, Layout::AxisValue) = after.axes[index];
                Field<double>(element, Layout::AxisTime) = time;
            }
            if (before.hat != after.hat) {
                const auto hats = Field<void*>(driver, Layout::Hats);
                const auto element = Field<void*>(hats, 0x20);
                Field<int>(element, Layout::HatValue) = after.hat;
                Field<double>(element, Layout::HatTime) = time;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void TraceRejectedState(void* driver) {
        // Only bounded layout metadata; no HID payload, device ID or account data.
        unsigned long long buttons = 0, axes = 0, hats = 0;
        int reportLength = 0;
        __try {
            if (driver) {
                const auto buttonArray = Field<void*>(driver, Layout::Buttons);
                const auto axisArray = Field<void*>(driver, Layout::Axes);
                const auto hatArray = Field<void*>(driver, Layout::Hats);
                const auto report = Field<void*>(driver, Layout::Report);
                if (buttonArray) buttons = Field<std::size_t>(buttonArray, 0x18);
                if (axisArray) axes = Field<std::size_t>(axisArray, 0x18);
                if (hatArray) hats = Field<std::size_t>(hatArray, 0x18);
                if (report) reportLength = Field<int>(report, 0x14);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { reportLength = -1; }
        char text[192]{};
        sprintf_s(text, "DualSense parsed state rejected: buttons=%llu axes=%llu hats=%llu reportLength=%d",
            buttons, axes, hats, reportLength);
        Log(text);
    }
    bool WINAPI HookParse(void* driver, const void* report, int length, double timestamp, void* methodInfo) {
        // The game's parser validates/decodes the original report first. Never
        // rewrite HID packets, CRCs, gyro, touch coordinates or output effects.
        const bool result = g_OriginalParse(driver, report, length, timestamp, methodInfo);
        if (g_Observed) g_Observed();
        if (!result || !std::isfinite(timestamp)) {
            if (!(g_Traced.fetch_or(8u, std::memory_order_relaxed) & 8u))
                Log("DualSense parser reached, but game rejected report or timestamp was invalid");
            return result;
        }
        if (!g_Wanted || !g_Wanted()) return result;
        std::lock_guard<std::mutex> lock(g_StateMutex);
        if (g_Disposing.find(driver) != g_Disposing.end()) return result;
        DualSenseState state;
        bool bluetooth = false;
        if (!ReadState(driver, state, bluetooth)) {
            if (!(g_Traced.fetch_or(16u, std::memory_order_relaxed) & 16u)) TraceRejectedState(driver);
            return result;
        }
        const unsigned bit = bluetooth ? 2u : 1u;
        if (!(g_Traced.fetch_or(bit, std::memory_order_relaxed) & bit))
            Log(bluetooth ? "Game reads native DualSense Bluetooth reports" : "Game reads native DualSense USB reports");
        auto pad = state.ToGamepad();
        if (g_Process && g_Process(driver, pad)) {
            auto mapped = state;
            mapped.ApplyRoutedPad(pad);
            if (!WriteChanges(driver, state, mapped, timestamp) &&
                !(g_Traced.fetch_or(4u, std::memory_order_relaxed) & 4u))
                Log("DualSense state write unavailable; native input layout needs verification");
        }
        return result;
    }
    void BeginDispose(void* driver) {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        ++g_Disposing[driver];
        if (g_Disconnect) g_Disconnect(driver);
    }
    void EndDispose(void* driver) {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        const auto found = g_Disposing.find(driver);
        if (found != g_Disposing.end() && !--found->second) g_Disposing.erase(found);
    }
    void WINAPI HookDispose(void* driver, bool disposing, void* methodInfo) {
        BeginDispose(driver);
        // No plugin mutex is held while the game tears down its HID reader.
        __try { g_OriginalDispose(driver, disposing, methodInfo); }
        __finally { EndDispose(driver); }
    }
    bool CheckBody(const unsigned char* parser, const unsigned char* dispose) {
        __try {
            if (parser[0x124] != 0xE8) return false;
            const auto buttons = parser + 0x129 + *reinterpret_cast<const int*>(parser + 0x125);
            const unsigned char arrayRead[] = {0x49, 0x8B, 0x46, 0x48};
            const unsigned char axesRead[] = {0x49, 0x8B, 0x5F, 0x30};
            const unsigned char hatsRead[] = {0x49, 0x8B, 0x5F, 0x20};
            const unsigned char valueWrite[] = {0x88, 0x56, 0x20, 0xF2, 0x0F, 0x11, 0x76, 0x28};
            const unsigned char disposed[] = {0x80, 0x79, 0x50, 0x00};
            DWORD64 base = 0;
            const auto parseFunction = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(parser), &base, nullptr);
            if (!parseFunction || base + parseFunction->BeginAddress != reinterpret_cast<DWORD64>(parser)) return false;
            const auto buttonFunction = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(buttons), &base, nullptr);
            if (!buttonFunction || base + buttonFunction->BeginAddress != reinterpret_cast<DWORD64>(buttons)) return false;
            const auto disposeFunction = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(dispose), &base, nullptr);
            return disposeFunction && base + disposeFunction->BeginAddress == reinterpret_cast<DWORD64>(dispose) &&
                !std::memcmp(parser + 0x129, axesRead, sizeof(axesRead)) &&
                !std::memcmp(parser + 0x191, hatsRead, sizeof(hatsRead)) &&
                !std::memcmp(buttons + 0x47, arrayRead, sizeof(arrayRead)) &&
                !std::memcmp(buttons + 0x7D, valueWrite, sizeof(valueWrite)) &&
                !std::memcmp(dispose + 0x15, disposed, sizeof(disposed));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

void DualSenseBackend::Configure(Logger log, Wanted wanted, Observed observed, Process process, Disconnect disconnect) {
    g_Log = log; g_Wanted = wanted; g_Observed = observed; g_Process = process; g_Disconnect = disconnect;
}
void DualSenseBackend::Prepare(Resolver resolve) {
    if (g_Prepared) return;
    g_Prepared = true;
    g_ParseEntry = resolve(ParseSignature);
    g_DisposeEntry = resolve(DisposeSignature);
    if (!g_ParseEntry || !g_DisposeEntry || !CheckBody(static_cast<const unsigned char*>(g_ParseEntry),
        static_cast<const unsigned char*>(g_DisposeEntry))) {
        g_ParseEntry = nullptr; g_DisposeEntry = nullptr;
        Log("DualSense driver signatures/layout absent or ambiguous; native adapter inactive");
    }
}
void DualSenseBackend::Install() {
    if (!g_ParseEntry || !g_DisposeEntry || g_InstallAttempted) return;
    g_InstallAttempted = true;
    if (MH_CreateHook(g_ParseEntry, reinterpret_cast<void*>(&HookParse),
        reinterpret_cast<void**>(&g_OriginalParse)) != MH_OK ||
        MH_CreateHook(g_DisposeEntry, reinterpret_cast<void*>(&HookDispose),
            reinterpret_cast<void**>(&g_OriginalDispose)) != MH_OK ||
        MH_EnableHook(g_DisposeEntry) != MH_OK || MH_EnableHook(g_ParseEntry) != MH_OK) {
        if (g_OriginalParse) { MH_DisableHook(g_ParseEntry); MH_RemoveHook(g_ParseEntry); }
        if (g_OriginalDispose) { MH_DisableHook(g_DisposeEntry); MH_RemoveHook(g_DisposeEntry); }
        Log("DualSense driver hooks unavailable; XInput backend remains available");
        return;
    }
    Log("Native DualSense USB/Bluetooth adapter ready; reports and output effects remain game-owned");
}
