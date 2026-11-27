#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include "MinHook.h"
#include "HotSwitch.h"
#include "DeviceActivity.h"
#include "GameBuild.h"

namespace {
    // Current native input-manager UI update. Its original finishes walking
    // input handlers before we request a device change, including menu pages.
    constexpr const char* UpdateSignature =
        "56 57 53 48 83 EC 20 48 89 CE 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
        "80 BE ? ? ? ? 00 74 08 48 83 C4 20 5B 5F 5E C3 "
        "80 3D ? ? ? ? 00 0F 85 ? ? ? ? 80 BE ? ? ? ? 00 74 ? "
        "48 89 F1 E8 ? ? ? ? 48 85 C0 74 ? 83 BE ? ? ? ? 00 75 ? "
        "8B 86 ? ? ? ? 83 F8 23 74 ? 83 F8 0B 75 ? B9 1B 00 00 00 E8 ? ? ? ? "
        "84 C0 74 ? 48 8B 05 ? ? ? ? 48 8B B8 ? ? ? ? 48 85 FF";
    constexpr const char* UiModeSignature =
        "55 56 57 53 48 83 EC 58 48 8D 6C 24 50 48 C7 45 F8 FE FF FF FF "
        "44 89 C3 41 89 D0 48 89 CE 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
        "44 39 86 ? ? ? ? 0F 84 ? ? ? ? 44 89 86 ? ? ? ? 48 89 F1 E8 ? ? ? ?";
    constexpr const char* InputModeSignature =
        "56 57 53 48 83 EC 70 44 89 C3 89 D7 48 89 CE 80 3D ? ? ? ? 00 "
        "0F 85 ? ? ? ? 8B 86 ? ? ? ? 39 F8 75 08 48 83 C4 70 5B 5F 5E C3";
    constexpr const char* CursorSignature =
        "48 83 EC 28 48 89 CA 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
        "8B 82 ? ? ? ? 85 C0 74 ? 83 F8 03 74 ? 83 F8 02 75 ? "
        "48 8B 05 ? ? ? ? 48 8B 88 ? ? ? ? 48 85 C9";
    // The native settings-page keyboard switch. Only decode its shared
    // callees/metadata; never invoke the wrapper with a fabricated receiver.
    // Its settings child distinguishes it from the second native wrapper.
    constexpr const char* SwitchSequencePrefix =
        "48 8B 15 ? ? ? ? E8 ? ? ? ? 48 89 C7 48 8B 05 ? ? ? ? "
        "48 8B 88 ? ? ? ? 48 85 C9 0F 84 ? ? ? ? BA 01 00 00 00 41 B0 01 E8 ? ? ? ? "
        "48 8B 05 ? ? ? ? 48 8B 88 ? ? ? ? 48 85 C9 0F 84 ? ? ? ? "
        "BA 02 00 00 00 45 31 C0 E8 ? ? ? ? 48 85 FF 74 ? "
        "48 8B 05 ? ? ? ? 48 8B 88 ? ? ? ? 48 85 C9 0F 84 ? ? ? ? E8 ? ? ? ? "
        "80 3D ? ? ? ? 00 0F 85 ? ? ? ? 48 89 F9 89 C2 E8 ? ? ? ? "
        "48 8B 05 ? ? ? ? 48 8B 88 ? ? ? ? 48 85 C9 0F 84 ? ? ? ? E8 ? ? ? ? "
        "48 8B 8E ";
    // Native UI-manager device transition, including registered child pages.
    // These are shared functions, not a settings-page wrapper or a page reopen.
    constexpr const char* BeginMenusSignature =
        "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 38 45 89 C4 41 89 D5 48 89 CE "
        "80 3D ? ? ? ? 00 0F 84 ? ? ? ? 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
        "48 8B 8E ? ? ? ? 48 85 C9 0F 84 ? ? ? ? 48 8B 15 ? ? ? ? E8 ? ? ? ? "
        "48 85 C0 0F 84 ? ? ? ? 48 89 C7 8B 58 18 FF CB";
    constexpr const char* FinishMenusSignature =
        "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 28 45 89 C4 41 89 D5 48 89 CB "
        "80 3D ? ? ? ? 00 0F 84 ? ? ? ? 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
        "48 8B 8B ? ? ? ? 48 85 C9 0F 84 ? ? ? ? 48 8B 15 ? ? ? ? E8 ? ? ? ? "
        "48 85 C0 0F 84 ? ? ? ? 48 89 C6 48 8B 40 18 85 C0";
    constexpr const char* NativeManagerAccessSignature =
        "55 41 56 56 57 53 48 81 EC C0 00 00 00 48 8D AC 24 80 00 00 00 "
        "0F 29 75 30 48 C7 45 20 FE FF FF FF 45 89 C6 89 D3 48 89 CF "
        "80 3D ? ? ? ? 00 0F 84 ? ? ? ? 80 3D ? ? ? ? 00 "
        "48 8B 05 ? ? ? ? 0F 85 ? ? ? ? 48 8B 88 ? ? ? ? "
        "48 85 C9 0F 84 ? ? ? ? E8 ? ? ? ? 84 C0";
    using Update = void (WINAPI*)(void*, void*);
    using SetMode = void (WINAPI*)(void*, int, bool, void*);
    using GetPage = void* (WINAPI*)(void*, void*);
    using GetKind = int (WINAPI*)(void*, void*);
    using RefreshHud = void (WINAPI*)(void*, int, void*);
    using TransitionMenus = void (WINAPI*)(void*, int, int, void*);
    using RecalculateCursor = void (WINAPI*)(void*, bool, void*);
    using InputHotSwap = void (WINAPI*)(void*, bool, int, void*);
    using NativeCursorSetter = void (WINAPI*)(void*, int, bool, void*);
    // UnityEngine.Object.get_name() returns a managed UTF-16 string.
    struct ManagedName { void* klass; void* monitor; int length; wchar_t chars[1]; };
    static_assert(offsetof(ManagedName, length) == 0x10 && offsetof(ManagedName, chars) == 0x14);
    using GetObjectName = ManagedName* (WINAPI*)(void*, void*);
    struct WorldCursorLayout {
        unsigned pageStack, regularPages, overlayPages;
        unsigned requestList, requestHead, requestNext, pageStateSlot;
        unsigned uiCursorMode, pageCursorRequest;
        unsigned pageProxy, pageObject, pageActivation, pageLoadState, pageNavigation;
        std::uintptr_t cursorSetterRva, cursorApplyRva, nameThunkRva, nameTargetRva;
    };
    // The two builds use the same cursor arbitration code with different object layouts.
    constexpr WorldCursorLayout CnWorldCursor{
        0x1E8, 0x240, 0x270,
        0x98, 0x18, 0x18, 0x160,
        0x310, 0x1E8,
        0x68, 0x88, 0x1DC, 0x1EC, 0x1C4,
        0xA210650, 0xA2065C0, 0x1A226A50, 0x1449820,
    };
    constexpr WorldCursorLayout OsWorldCursor{
        0x38, 0x1A8, 0x288,
        0x1A0, 0x10, 0x28, 0x2D0,
        0x334, 0x1C4,
        0x58, 0x10, 0x1F0, 0x1F8, 0x1C8,
        0xA1FEEC0, 0xA1F2F40, 0x1A258CF0, 0x144A820,
    };
    Update g_OriginalUpdate = nullptr, g_Cursor = nullptr;
    SetMode g_SetUiMode = nullptr, g_SetInputMode = nullptr;
    GetPage g_GetHud = nullptr;
    GetKind g_GetUiKind = nullptr;
    RefreshHud g_RefreshHud = nullptr;
    TransitionMenus g_BeginMenus = nullptr, g_FinishMenus = nullptr;
    RecalculateCursor g_RecalculateCursor = nullptr;
    InputHotSwap g_InputHotSwap = nullptr;
    NativeCursorSetter g_NativeCursorSetter = nullptr;
    GetObjectName g_GetObjectName = nullptr;
    void** g_HudMethodSlot = nullptr;
    void* g_UpdateEntry = nullptr;
    void** g_ManagerSlot = nullptr;
    unsigned g_UiManagerOffset = 0, g_InputManagerOffset = 0, g_InputBlockedOffset = 0;
    unsigned g_InputModeOffset = 0, g_UiModeOffset = 0;
    unsigned g_UiKindOffset = 0, g_PagesOffset = 0, g_PageRefreshOffset = 0, g_ReferenceOffset = 0;
    unsigned g_PageCountOffset = 0, g_PageItemsOffset = 0;
    unsigned g_PageReadyOffset = 0, g_PageDirtyOffset = 0, g_PageModeStateOffset = 0;
    unsigned g_PreviousUiOffset = 0, g_PreviousInputOffset = 0, g_TargetUiOffset = 0, g_TargetInputOffset = 0;
    HotSwitch::Logger g_Log = nullptr;
    bool g_Diagnostics = false;
    unsigned g_LayoutTraces = 0;
    std::atomic<bool> g_Enabled{ false }, g_Ready{ false };
    bool g_Prepared = false, g_InstallAttempted = false;
    const WorldCursorLayout* g_WorldCursorLayout = nullptr;
    DeviceActivity g_Activity;
    GamepadActivity g_Pads;
    HWND g_Window = nullptr;
    bool g_WindowAttempted = false, g_MouseSeen = false, g_RawMouseSeen = false, g_FirstTick = true;
    LPARAM g_MousePosition = 0;
    ULONGLONG g_IgnoreMouseUntil = 0, g_LastSwitchTime = 0;
    std::uint64_t g_Handled = 0;
    thread_local bool g_Updating = false;
    void* g_MainHud = nullptr;
    ULONGLONG g_CursorRepairUntil = 0, g_CursorRepairLast = 0;
    unsigned g_CursorRepairAttempts = 0;
    bool g_WorldCursorGuard = false;
    ULONGLONG g_WorldCursorCheckedAt = 0;
    bool g_NativeWorldCursorOverride = false;
    void* g_NativeWorldCursorUi = nullptr;

    void Log(const char* text) { if (g_Log) g_Log(text); }
    const unsigned char* Relative(const unsigned char* displacement) {
        return displacement + 4 + *reinterpret_cast<const int*>(displacement);
    }
    const unsigned char* CallTarget(const unsigned char* call) {
        return call[0] == 0xE8 ? Relative(call + 1) : nullptr;
    }
    // Both builds load the same page collection. The OS build uses disp8.
    unsigned MemberLoadOffset(const unsigned char* load, unsigned registers) {
        if (load[0] != 0x48 || load[1] != 0x8B) return 0;
        if (load[2] == (0x80 | registers)) return *reinterpret_cast<const unsigned*>(load + 3);
        if (load[2] == (0x40 | registers) && load[3] < 0x80) return load[3];
        return 0;
    }
    std::size_t GetImageSize(const unsigned char* imageBase) {
        if (!imageBase) return 0;
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
            dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) ||
            dos->e_lfanew > 0x1000) return 0;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
        return nt->OptionalHeader.SizeOfImage;
    }
    bool InImageRange(std::size_t imageSize, std::uintptr_t rva, std::size_t length) {
        return rva <= imageSize && length <= imageSize - rva;
    }
    bool PointerInImageRange(const unsigned char* imageBase, std::size_t imageSize,
        const unsigned char* pointer, std::size_t length) {
        const auto base = reinterpret_cast<std::uintptr_t>(imageBase);
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        return base && address >= base && InImageRange(imageSize, address - base, length);
    }
    bool Focused() {
        DWORD pid = 0;
        const HWND window = GetForegroundWindow();
        return window && GetWindowThreadProcessId(window, &pid) && pid == GetCurrentProcessId();
    }
    void* LookupMainHud(void* ui) {
        if (!ui || !g_GetHud || !g_HudMethodSlot) return nullptr;
        __try {
            if (!*g_HudMethodSlot) return nullptr;
            void* hud = g_GetHud(ui, *g_HudMethodSlot);
            g_MainHud = hud;
            return hud;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    bool OnlyLockedWorldHud(const unsigned char* ui) {
        if (!g_WorldCursorLayout || !ui || !g_PageCountOffset || !g_PageItemsOffset) return false;
        const auto& layout = *g_WorldCursorLayout;
        void* const hud = LookupMainHud(const_cast<unsigned char*>(ui));
        if (!hud) return false;
        __try {
            const auto stack = *reinterpret_cast<const unsigned char* const*>(ui + g_PagesOffset);
            const auto regular = *reinterpret_cast<const unsigned char* const*>(ui + layout.regularPages);
            const auto overlay = *reinterpret_cast<const unsigned char* const*>(ui + layout.overlayPages);
            if (!stack || !regular || !overlay ||
                *reinterpret_cast<const int*>(stack + g_PageCountOffset) != 1 ||
                *reinterpret_cast<const int*>(regular + 0x18) != 0 ||
                *reinterpret_cast<const int*>(overlay + 0x18) != 1) return false;
            const auto array = *reinterpret_cast<const unsigned char* const*>(stack + g_PageItemsOffset);
            if (!array || *reinterpret_cast<const std::size_t*>(array + 0x18) < 1)
                return false;
            const auto top = *reinterpret_cast<const unsigned char* const*>(array + 0x20);
            return top == hud &&
                *reinterpret_cast<const int*>(top + layout.pageCursorRequest) == -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool IsKeyboardWorldUi(bool requireCachedLocked) {
        if (!g_WorldCursorLayout) return false;
        __try {
            if (!g_ManagerSlot || !*g_ManagerSlot ||
                !g_InputManagerOffset || !g_UiManagerOffset ||
                !g_InputBlockedOffset || !g_InputModeOffset || !g_UiModeOffset) return false;
            const auto root = static_cast<const unsigned char*>(*g_ManagerSlot);
            const auto input = *reinterpret_cast<const unsigned char* const*>(
                root + g_InputManagerOffset);
            const auto ui = *reinterpret_cast<const unsigned char* const*>(
                root + g_UiManagerOffset);
            return input && ui &&
                !*reinterpret_cast<const unsigned char*>(input + g_InputBlockedOffset) &&
                *reinterpret_cast<const int*>(input + g_InputModeOffset) == 2 &&
                *reinterpret_cast<const int*>(ui + g_UiModeOffset) == 1 &&
                (!requireCachedLocked || *reinterpret_cast<const int*>(ui + g_WorldCursorLayout->uiCursorMode) == -1) &&
                OnlyLockedWorldHud(ui);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool KeyboardCursorLocked() { return IsKeyboardWorldUi(true); }
    bool KeyboardWorldCursorLocked(ULONGLONG now) {
        return g_WorldCursorLayout && g_WorldCursorGuard && g_WorldCursorCheckedAt &&
            now - g_WorldCursorCheckedAt <= 150 && IsKeyboardWorldUi(false);
    }
    void ReadGameObjectName(const void* obj, char (&name)[160]) {
        name[0] = '\0';
        if (!obj || !g_GetObjectName) return;
        __try {
            const auto result = g_GetObjectName(const_cast<void*>(obj), nullptr);
            if (!result || result->length < 1 || result->length > 79) return;
            const int size = WideCharToMultiByte(CP_UTF8, 0, result->chars,
                result->length, name, static_cast<int>(sizeof(name) - 1), nullptr, nullptr);
            if (size > 0) name[size] = '\0';
            else name[0] = '\0';
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { name[0] = '\0'; }
    }
    bool WorldHudIsEffectiveCursorOwner(const unsigned char* ui) {
        if (!g_GetObjectName || !OnlyLockedWorldHud(ui)) return false;
        const auto& layout = *g_WorldCursorLayout;
        // The one reproducible world-only stray requester is a BeyondClient
        // joypad navigation dialog. When the actual UI stack is only the locked
        // main HUD, its Normal request must not force a Windows arrow on-screen.
        using PageState = int(WINAPI*)(const void*, void*);
        __try {
            const auto list = *reinterpret_cast<const unsigned char* const*>(ui + layout.requestList);
            if (!list) return false;
            const auto head = *reinterpret_cast<const unsigned char* const*>(list + layout.requestHead);
            if (!head) return false;
            auto node = head;
            bool foundKnownDialog = false;
            bool foundActiveHud = false;
            constexpr unsigned MaxCursorNodes = 2048;
            for (unsigned i = 0; i < MaxCursorNodes; ++i) {
                if (*reinterpret_cast<const void* const*>(node + 0x10) != list)
                    return false;
                const auto page = *reinterpret_cast<const unsigned char* const*>(node + 0x20);
                if (!page) return false;
                const auto klass = *reinterpret_cast<const unsigned char* const*>(page);
                if (!klass) return false;
                const auto getter = *reinterpret_cast<PageState const*>(klass + layout.pageStateSlot);
                if (!getter) return false;
                const int state = getter(page, nullptr);
                const int request = *reinterpret_cast<const int*>(page + layout.pageCursorRequest);
                if (request != -1 && request != 0 && request != 1000) return false;
                if (state == 5 && request != 0)
                    return page == g_MainHud && request == -1;
                if (state != 0 && !(state == 5 && request == 0)) {
                    if (request == 1000) {
                        if (foundKnownDialog || state != 2) return false;
                        const auto proxy = *reinterpret_cast<const void* const*>(page + layout.pageProxy);
                        const auto object = *reinterpret_cast<const void* const*>(page + layout.pageObject);
                        if (proxy || !object ||
                            *reinterpret_cast<const int*>(page + layout.pageActivation) != 0 ||
                            *reinterpret_cast<const int*>(page + layout.pageLoadState) != 3 ||
                            *reinterpret_cast<const int*>(page + layout.pageNavigation) != 2)
                            return false;
                        char objectName[160]{};
                        ReadGameObjectName(object, objectName);
                        if (std::strcmp(objectName, "BeyondClientUIJoypadNavInputDialog") != 0)
                            return false;
                        foundKnownDialog = true;
                    } else if (request == -1 && page == g_MainHud) {
                        foundActiveHud = true;
                    }
                }
                const auto next = *reinterpret_cast<const unsigned char* const*>(node + layout.requestNext);
                if (!next) return false;
                if (next == head) return foundKnownDialog || foundActiveHud;
                node = next;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return false;
    }
    bool TryReadUiCursorMode(const void* ui, int& mode) {
        if (!g_WorldCursorLayout || !ui) return false;
        __try {
            mode = *reinterpret_cast<const int*>(static_cast<const unsigned char*>(ui) +
                g_WorldCursorLayout->uiCursorMode);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool TrySetNativeCursorLocked(void* ui) {
        if (!g_WorldCursorLayout || !g_NativeCursorSetter || !ui) return false;
        __try {
            g_NativeCursorSetter(ui, -1, false, nullptr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_NativeWorldCursorOverride = false;
            g_NativeWorldCursorUi = nullptr;
            g_WorldCursorGuard = false;
            g_WorldCursorCheckedAt = 0;
            return false;
        }
    }
    void RestoreNativeCursorArbitration(void* ui) {
        g_NativeWorldCursorOverride = false;
        g_NativeWorldCursorUi = nullptr;
        if (!g_WorldCursorLayout || !g_RecalculateCursor || !ui) return;
        __try {
            g_RecalculateCursor(ui, false, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_WorldCursorGuard = false;
            g_WorldCursorCheckedAt = 0;
        }
    }
    void TryRefreshCursor(void* instance) {
        if (!g_Cursor || !instance) return;
        __try { g_Cursor(instance, nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    void UpdateWorldCursorState(void* instance, void* ui, int current, int uiMode, ULONGLONG now) {
        if (!g_WorldCursorLayout) return;
        if (g_CursorRepairUntil && now >= g_CursorRepairUntil)
            g_CursorRepairUntil = 0;
        const bool keyboardForeground = Focused() && current == 2 && uiMode == 1;
        if (!keyboardForeground) {
            g_WorldCursorGuard = false;
            g_WorldCursorCheckedAt = 0;
            if (g_NativeWorldCursorOverride)
                RestoreNativeCursorArbitration(ui);
            return;
        }
        bool worldOwner = false;
        bool worldOwnerEvaluated = false;
        auto checkWorldOwner = [&]() {
            if (!worldOwnerEvaluated) {
                worldOwner = WorldHudIsEffectiveCursorOwner(static_cast<const unsigned char*>(ui));
                worldOwnerEvaluated = true;
                g_WorldCursorGuard = worldOwner;
                g_WorldCursorCheckedAt = now;
            }
            return worldOwner;
        };
        if (!g_WorldCursorCheckedAt || now - g_WorldCursorCheckedAt >= 100)
            checkWorldOwner();
        if (g_CursorRepairUntil && g_CursorRepairAttempts < 6 &&
            (!g_CursorRepairLast || now - g_CursorRepairLast >= 150) &&
            KeyboardCursorLocked()) {
            CURSORINFO info{ sizeof(info) };
            if (GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING)) {
                ++g_CursorRepairAttempts;
                g_CursorRepairLast = now;
                TryRefreshCursor(instance);
                CURSORINFO after{ sizeof(after) };
                if (GetCursorInfo(&after) && (after.flags & CURSOR_SHOWING) &&
                    KeyboardCursorLocked()) {
                    SetCursor(nullptr);
                }
            }
        }
        int cached = 0;
        if (g_NativeCursorSetter && OnlyLockedWorldHud(static_cast<const unsigned char*>(ui)) &&
            TryReadUiCursorMode(ui, cached) && cached != -1) {
            // Re-verify the live request list before forcing native Locked; never trust a 100ms cache.
            if (checkWorldOwner() && TrySetNativeCursorLocked(ui)) {
                g_NativeWorldCursorOverride = true;
                g_NativeWorldCursorUi = ui;
            }
        }
        if (g_NativeWorldCursorOverride &&
            (ui != g_NativeWorldCursorUi || !checkWorldOwner())) {
            RestoreNativeCursorArbitration(ui);
        }
    }
    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM key, LPARAM detail,
        UINT_PTR id, DWORD_PTR) {
        if (g_Enabled.load(std::memory_order_relaxed)) {
            const auto now = GetTickCount64();
            switch (message) {
            case WM_KILLFOCUS:
                g_CursorRepairUntil = 0;
                g_WorldCursorGuard = false;
                g_WorldCursorCheckedAt = 0;
                g_Activity.Clear();
                g_MouseSeen = false;
                g_RawMouseSeen = false;
                break;
            case WM_SETFOCUS:
                g_Activity.Clear();
                g_RawMouseSeen = false;
                g_IgnoreMouseUntil = now + 500;
                if (g_WorldCursorLayout) {
                    g_CursorRepairUntil = now + 2000;
                    g_CursorRepairAttempts = 0;
                    g_CursorRepairLast = 0;
                    g_WorldCursorGuard = false;
                    g_WorldCursorCheckedAt = 0;
                }
                break;
            case WM_SETCURSOR:
                // Windows can reset the pointer on focus/movement after the game
                // has already cached Locked. Intercept client-only repaint, not
                // menu cursors or non-client/window-border cursor messages.
                if (g_WorldCursorLayout && window == g_Window && LOWORD(detail) == HTCLIENT &&
                    Focused() && (KeyboardCursorLocked() || KeyboardWorldCursorLocked(now))) {
                    SetCursor(nullptr);
                    return TRUE;
                }
                break;
            case WM_INPUT: {
                RAWINPUT raw{};
                UINT size = sizeof(raw);
                if (GET_RAWINPUT_CODE_WPARAM(key) == RIM_INPUT && Focused() &&
                    GetRawInputData(reinterpret_cast<HRAWINPUT>(detail), RID_INPUT,
                        &raw, &size, sizeof(RAWINPUTHEADER)) == sizeof(raw) &&
                    raw.header.dwType == RIM_TYPEMOUSE) {
                    if (!(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) g_RawMouseSeen = true;
                    if (IsRawMouseActivity(raw.data.mouse))
                        g_Activity.Notify(InputSource::Mouse, now);
                }
                break;
            }
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (!(detail & (1LL << 30)) && Focused())
                    g_Activity.Notify(InputSource::Keyboard, now);
                break;
            case WM_MOUSEMOVE:
                if (!g_RawMouseSeen && g_MouseSeen && detail != g_MousePosition && now >= g_IgnoreMouseUntil && Focused())
                    g_Activity.Notify(InputSource::Mouse, now);
                g_MouseSeen = true;
                g_MousePosition = detail;
                break;
            case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
            case WM_XBUTTONDOWN: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                if (Focused()) g_Activity.Notify(InputSource::Mouse, now);
                break;
            }
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, WindowProc, id);
            g_Window = nullptr;
            g_Enabled.store(false, std::memory_order_release);
            g_WorldCursorGuard = false;
            g_WorldCursorCheckedAt = 0;
            g_NativeWorldCursorOverride = false;
            g_NativeWorldCursorUi = nullptr;
            g_MainHud = nullptr;
        }
        return DefSubclassProc(window, message, key, detail);
    }
    BOOL CALLBACK FindWindow(HWND window, LPARAM data) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(window)) return TRUE;
        wchar_t name[64]{};
        GetClassNameW(window, name, ARRAYSIZE(name));
        if (wcscmp(name, L"UnityWndClass")) return TRUE;
        *reinterpret_cast<HWND*>(data) = window;
        return FALSE;
    }
    bool InstallWindowMonitor() {
        if (g_Window) return true;
        if (g_WindowAttempted) return false;
        g_WindowAttempted = true;
        HWND window = nullptr;
        EnumWindows(FindWindow, reinterpret_cast<LPARAM>(&window));
        // SetWindowSubclass must execute on the window's owning thread.
        if (!window || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId() ||
            !SetWindowSubclass(window, WindowProc, reinterpret_cast<UINT_PTR>(&g_Activity), 0)) {
            Log("Hot switch window/thread check failed; feature disabled, no cross-thread subclass");
            g_Enabled.store(false, std::memory_order_release);
            return false;
        }
        g_Window = window;
        Log("Hot switch window input monitor installed on game UI thread");
        return true;
    }
    bool ReadManagers(void* instance, void*& ui, int& inputMode, int& uiMode) {
        __try {
            const auto root = static_cast<unsigned char*>(*g_ManagerSlot);
            if (!root || !instance ||
                *reinterpret_cast<void**>(root + g_InputManagerOffset) != instance) return false;
            ui = *reinterpret_cast<void**>(root + g_UiManagerOffset);
            if (!ui) return false;
            // The native update itself bails out while input is paused; keep ui
            // populated so HookUpdate can still restore native cursor arbitration.
            if (*(static_cast<unsigned char*>(instance) + g_InputBlockedOffset)) return false;
            inputMode = *reinterpret_cast<int*>(static_cast<unsigned char*>(instance) + g_InputModeOffset);
            uiMode = *reinterpret_cast<int*>(static_cast<unsigned char*>(ui) + g_UiModeOffset);
            return (inputMode == 2 || inputMode == 3) && (uiMode == 1 || uiMode == 2);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void TraceLayout(void* ui, bool hadHud) {
        // Read only the native open-page collection on the first eight device
        // changes. No timer, scene search or object-name lookup.
        if (!g_Diagnostics || g_LayoutTraces >= 8) return;
        ++g_LayoutTraces;
        __try {
            const auto bytes = static_cast<const unsigned char*>(ui);
            const auto reference = reinterpret_cast<const float*>(bytes + g_ReferenceOffset);
            const auto pages = *reinterpret_cast<const unsigned char* const*>(bytes + g_PagesOffset);
            const int count = pages ? *reinterpret_cast<const int*>(pages + g_PageCountOffset) : 0;
            char text[4096]{};
            const int written = sprintf_s(text, "Hot switch layout: HUD refreshed=%u; kind=%d; reference=%.0fx%.0f; open pages=%d",
                hadHud ? 1u : 0u, *reinterpret_cast<const int*>(bytes + g_UiKindOffset),
                static_cast<double>(reference[0]), static_cast<double>(reference[1]), count);
            if (written < 0) return;
            if (count < 0 || count > 32) { Log(text); return; }
            const auto array = pages ? *reinterpret_cast<const unsigned char* const*>(pages + g_PageItemsOffset) : nullptr;
            if (!array || *reinterpret_cast<const std::size_t*>(array + 0x18) < static_cast<unsigned>(count)) { Log(text); return; }
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::size_t used = static_cast<std::size_t>(written);
            for (int index = count - 1; index >= 0; --index) {
                const auto page = *reinterpret_cast<const unsigned char* const*>(array + 0x20 + index * sizeof(void*));
                if (!page) continue;
                const auto klass = *reinterpret_cast<const unsigned char* const*>(page);
                if (!klass) continue;
                const auto callback = *reinterpret_cast<const std::uintptr_t*>(klass + g_PageRefreshOffset);
                const auto state = *reinterpret_cast<const unsigned char* const*>(page + g_PageModeStateOffset);
                if (used + 128 >= sizeof(text)) break;
                const int appended = sprintf_s(text + used, sizeof(text) - used,
                    "; page[%d] callback=0x%llX ready=%d dirty=%u previous=%d/%d target=%d/%d", index,
                    static_cast<unsigned long long>(callback >= base ? callback - base : 0),
                    *reinterpret_cast<const int*>(page + g_PageReadyOffset), static_cast<unsigned>(page[g_PageDirtyOffset]),
                    state ? *reinterpret_cast<const int*>(state + g_PreviousUiOffset) : -1,
                    state ? *reinterpret_cast<const int*>(state + g_PreviousInputOffset) : -1,
                    state ? *reinterpret_cast<const int*>(state + g_TargetUiOffset) : -1,
                    state ? *reinterpret_cast<const int*>(state + g_TargetInputOffset) : -1);
                if (appended < 0) break;
                used += static_cast<std::size_t>(appended);
            }
            Log(text); // One file write per captured device change, not per page.
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("Hot switch layout trace unavailable; no retry/search"); }
    }
    bool Apply(void* instance, void* ui, int mode, bool& hadHud) {
        const char* stage = "HUD lookup";
        __try {
            // Match the native switch order. It captures the HUD before changing
            // modes and refreshes its layout/button group afterwards explicitly.
            // The UI setter's generic page notification does not replace this.
            if (!*g_HudMethodSlot) { Log("Hot switch native HUD MethodInfo unavailable; switch skipped"); return false; }
            void* hud = g_GetHud(ui, *g_HudMethodSlot);
            g_MainHud = hud;
            const int uiMode = mode == 3 ? 2 : 1;
            // Capture old device state before either global mode is changed.
            // Generic resize notifications alone do not prepare menu controls.
            stage = "menu transition begin";
            g_BeginMenus(ui, uiMode, mode, nullptr);
            stage = "UI mode";
            // The native global transition passes false here: it uses the
            // menu begin/finish pair, not generic callbacks while enumerating
            // the live page collection. Resolution/kind still update normally.
            g_SetUiMode(ui, uiMode, false, nullptr);
            stage = "input mode";
            g_SetInputMode(instance, mode, false, nullptr);
            if (hud) {
                stage = "HUD refresh";
                g_RefreshHud(hud, g_GetUiKind(ui, nullptr), nullptr);
                hadHud = true;
            }
            stage = "cursor refresh";
            g_Cursor(instance, nullptr);
            if (g_RecalculateCursor) {
                stage = "native cursor arbitration";
                g_RecalculateCursor(ui, false, nullptr);
            }
            if (g_InputHotSwap) {
                stage = "native InputHotSwap begin";
                g_InputHotSwap(ui, false, 4, nullptr);
            }
            stage = "menu transition finish";
            g_FinishMenus(ui, uiMode, mode, nullptr);
            if (g_InputHotSwap) {
                stage = "native InputHotSwap finish";
                g_InputHotSwap(ui, true, 4, nullptr);
            }
            return *reinterpret_cast<int*>(static_cast<unsigned char*>(instance) + g_InputModeOffset) == mode &&
                *reinterpret_cast<int*>(static_cast<unsigned char*>(ui) + g_UiModeOffset) == uiMode;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            char text[160]{};
            sprintf_s(text, "Hot switch native exception at %s; code=0x%08lX", stage, GetExceptionCode());
            Log(text);
            return false;
        }
    }
    void WINAPI HookUpdate(void* instance, void* methodInfo) {
        if (g_Updating) { g_OriginalUpdate(instance, methodInfo); return; }
        g_Updating = true;
        __try { g_OriginalUpdate(instance, methodInfo); }
        __finally { g_Updating = false; }
        if (!g_Enabled.load(std::memory_order_relaxed) || !InstallWindowMonitor()) return;
        void* ui = nullptr;
        int current = 0, uiMode = 0;
        if (!ReadManagers(instance, ui, current, uiMode)) {
            if (g_WorldCursorLayout) {
                g_WorldCursorGuard = false;
                g_WorldCursorCheckedAt = 0;
                if (g_NativeWorldCursorOverride) {
                    if (ui && ui == g_NativeWorldCursorUi) RestoreNativeCursorArbitration(ui);
                    else {
                        g_NativeWorldCursorOverride = false;
                        g_NativeWorldCursorUi = nullptr;
                    }
                }
            }
            return;
        }
        if (g_FirstTick) {
            char text[160]{};
            sprintf_s(text, "Hot switch native UI update observed; thread=%lu; input mode=%d; UI mode=%d",
                GetCurrentThreadId(), current, uiMode);
            Log(text);
            g_FirstTick = false;
        }
        const auto now = GetTickCount64();
        if (g_WorldCursorLayout)
            UpdateWorldCursorState(instance, ui, current, uiMode, now);
        const auto token = g_Activity.Peek(now);
        if (!token || token == g_Handled) return;
        const int requested = DeviceActivity::NativeMode(token);
        if (current == requested && uiMode == (requested == 3 ? 2 : 1)) { g_Handled = token; return; }
        if (now - g_LastSwitchTime < 500) return; // Preserve held input; avoid mode ping-pong.
        g_Handled = token;
        if (!Focused()) return;
        // Cursor recentering during the native switch must not count as mouse use.
        g_IgnoreMouseUntil = now + 500;
        if (g_WorldCursorLayout) {
            g_WorldCursorGuard = false;
            g_WorldCursorCheckedAt = 0;
            g_NativeWorldCursorOverride = false;
            g_NativeWorldCursorUi = nullptr;
        }
        g_Updating = true;
        bool hadHud = false;
        const bool success = Apply(instance, ui, requested, hadHud);
        g_Updating = false;
        if (!success) {
            Log("Hot switch native call failed or modes mismatched; feature disabled, no repeated retry");
            g_Enabled.store(false, std::memory_order_release);
            return;
        }
        g_LastSwitchTime = now;
        if (g_WorldCursorLayout && current == 3 && requested == 2) {
            g_CursorRepairUntil = now + 2000;
            g_CursorRepairAttempts = 0;
            g_CursorRepairLast = 0;
        }
        char text[160]{};
        sprintf_s(text, "Hot switch applied %d -> %d; source=%u; UI thread=%lu", current, requested,
            static_cast<unsigned>(DeviceActivity::Source(token)), GetCurrentThreadId());
        Log(text);
        TraceLayout(ui, hadHud);
    }
    void PrepareWorldCursorFix(const GameBuild::Profile& build,
        const unsigned char* nativeManager, const unsigned char* begin) {
        const bool overseas = build.region == GameBuild::Region::OS;
        const auto& layout = overseas ? OsWorldCursor : CnWorldCursor;
        const auto imageBase = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        const std::size_t imageSize = GetImageSize(imageBase);
        const auto cursorEntry = reinterpret_cast<const unsigned char*>(g_Cursor);
        constexpr unsigned char nameTargetEntry[] = { 0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00 };
        if (imageSize != build.imageSize ||
            !PointerInImageRange(imageBase, imageSize, nativeManager, 0x587) ||
            !PointerInImageRange(imageBase, imageSize, cursorEntry, 0x48) ||
            !InImageRange(imageSize, layout.cursorSetterRva, 0x46) ||
            !InImageRange(imageSize, layout.cursorApplyRva, 1) ||
            !InImageRange(imageSize, layout.nameThunkRva, 5) ||
            !InImageRange(imageSize, layout.nameTargetRva, sizeof(nameTargetEntry))) {
            Log("Hot switch cursor entry out of PE bounds; overworld cursor fix disabled");
            return;
        }

        // Both native transactions call the same sequence with region-specific targets.
        const auto recalc = CallTarget(nativeManager + 0x516);
        const auto hotSwap = CallTarget(nativeManager + 0x53A);
        const auto applyCursor = imageBase + layout.cursorApplyRva;
        const unsigned char beforeHotSwap[] = { 0x41, 0xB8, 0x04, 0x00, 0x00, 0x00 };
        const unsigned char afterHotSwap[] = { 0xB2, 0x01, 0x41, 0xB8, 0x04, 0x00, 0x00, 0x00 };
        if (!recalc || !hotSwap ||
            !PointerInImageRange(imageBase, imageSize, recalc, 0x1CD) ||
            !PointerInImageRange(imageBase, imageSize, hotSwap, 1) ||
            CallTarget(nativeManager + 0x4F8) != cursorEntry ||
            CallTarget(nativeManager + 0x55E) != reinterpret_cast<const unsigned char*>(g_FinishMenus) ||
            CallTarget(nativeManager + 0x582) != hotSwap ||
            cursorEntry[0x43] != 0xE9 || recalc[0x1C8] != 0xE9 ||
            Relative(cursorEntry + 0x44) != applyCursor || Relative(recalc + 0x1C9) != applyCursor ||
            nativeManager[0x514] != 0x31 || nativeManager[0x515] != 0xD2 ||
            std::memcmp(nativeManager + 0x534, beforeHotSwap, sizeof(beforeHotSwap)) != 0 ||
            std::memcmp(nativeManager + 0x57A, afterHotSwap, sizeof(afterHotSwap)) != 0) {
            Log("Hot switch cursor transaction mismatch; overworld cursor fix disabled");
            return;
        }

        const auto setter = imageBase + layout.cursorSetterRva;
        if (setter[0x32] != 0x89 || setter[0x33] != 0xB7 ||
            *reinterpret_cast<const unsigned*>(setter + 0x34) != layout.uiCursorMode ||
            setter[0x41] != 0xE9 || Relative(setter + 0x42) != applyCursor) {
            Log("Hot switch native cursor setter mismatch; overworld cursor fix disabled");
            return;
        }

        const auto nameThunk = imageBase + layout.nameThunkRva;
        const auto nameTarget = imageBase + layout.nameTargetRva;
        if (nameThunk[0] != 0xE9 || Relative(nameThunk + 1) != nameTarget ||
            std::memcmp(nameTarget, nameTargetEntry, sizeof(nameTargetEntry)) != 0) {
            Log("Hot switch object name getter mismatch; overworld cursor fix disabled");
            return;
        }

        if (g_PagesOffset != layout.pageStack ||
            MemberLoadOffset(begin + 0x33, 0x0E) != layout.regularPages ||
            MemberLoadOffset(begin + 0x98, 0x0E) != layout.overlayPages) {
            Log("Hot switch cursor page collections mismatch; overworld cursor fix disabled");
            return;
        }

        // Cross-check the fields and virtual getter used when walking native cursor requests.
        if (MemberLoadOffset(recalc + 0x3F, 0x06) != layout.requestList ||
            MemberLoadOffset(recalc + 0x4F, 0x28) != layout.requestHead ||
            MemberLoadOffset(recalc + 0x70, 0x3D) != 0x20 ||
            MemberLoadOffset(recalc + 0xB0, 0x05) != layout.requestNext ||
            MemberLoadOffset(recalc + 0xDF, 0x0D) != 0x10 ||
            recalc[0x85] != 0xFF || recalc[0x86] != 0x90 ||
            *reinterpret_cast<const unsigned*>(recalc + 0x87) != layout.pageStateSlot ||
            recalc[0x9D] != 0x8B || recalc[0x9E] != 0xBF ||
            *reinterpret_cast<const unsigned*>(recalc + 0x9F) != layout.pageCursorRequest) {
            Log("Hot switch cursor request layout mismatch; overworld cursor fix disabled");
            return;
        }

        g_RecalculateCursor = reinterpret_cast<RecalculateCursor>(const_cast<unsigned char*>(recalc));
        g_InputHotSwap = reinterpret_cast<InputHotSwap>(const_cast<unsigned char*>(hotSwap));
        g_NativeCursorSetter = reinterpret_cast<NativeCursorSetter>(const_cast<unsigned char*>(setter));
        g_GetObjectName = reinterpret_cast<GetObjectName>(const_cast<unsigned char*>(nameThunk));
        g_WorldCursorLayout = &layout;
        Log(overseas ? "Hot switch OS7.2 overworld cursor fix ready" : "Hot switch CN7.2 overworld cursor fix ready");
    }
}

void HotSwitch::Configure(Logger log, bool enabled, bool diagnostics) {
    g_Log = log; g_Enabled.store(enabled); g_Diagnostics = diagnostics;
}
bool HotSwitch::IsEnabled() { return g_Enabled.load(std::memory_order_relaxed); }
bool HotSwitch::IsReady() { return g_Ready.load(std::memory_order_acquire); }
void HotSwitch::ObserveXInput(unsigned index, const XINPUT_GAMEPAD& pad) {
    if (IsEnabled() && g_Pads.Sample(index, pad)) NotifyGamepadActivity();
}
void HotSwitch::NotifyGamepadActivity() {
    if (IsEnabled() && Focused()) g_Activity.Notify(InputSource::Gamepad, GetTickCount64());
}
void HotSwitch::DisconnectXInput(unsigned index) { g_Pads.Disconnect(index); }

void HotSwitch::Prepare(Resolver resolve, const GameBuild::Profile& build) {
    if (!IsEnabled() || g_Prepared) return;
    g_Prepared = true;
    g_UpdateEntry = resolve(UpdateSignature);
    g_SetUiMode = reinterpret_cast<SetMode>(resolve(UiModeSignature));
    g_SetInputMode = reinterpret_cast<SetMode>(resolve(InputModeSignature));
    g_Cursor = reinterpret_cast<Update>(resolve(CursorSignature));
    g_BeginMenus = reinterpret_cast<TransitionMenus>(resolve(BeginMenusSignature));
    g_FinishMenus = reinterpret_cast<TransitionMenus>(resolve(FinishMenusSignature));
    // The two native wrappers share HUD/mode/cursor callees. Match the
    // region's two distinct child fields and require both wrappers to agree.
    const bool overseas = build.region == GameBuild::Region::OS;
    const std::string prefix = SwitchSequencePrefix;
    const auto sequence = static_cast<const unsigned char*>(resolve(
        (prefix + (overseas ? "E8 02 00 00 48 85 C9" : "88 03 00 00 48 85 C9")).c_str()));
    const auto otherSequence = static_cast<const unsigned char*>(resolve(
        (prefix + (overseas ? "D8 02 00 00 48 85 C9" : "A0 02 00 00 48 85 C9")).c_str()));
    const auto nativeManager = static_cast<const unsigned char*>(resolve(NativeManagerAccessSignature));
    if (!g_UpdateEntry || !g_SetUiMode || !g_SetInputMode || !g_Cursor || !sequence || !otherSequence || !nativeManager || !g_BeginMenus || !g_FinishMenus) {
        Log("Hot switch signature absent/ambiguous; feature disabled, no fallback scan");
        g_Enabled.store(false);
        return;
    }
    // Decode fields from the verified native bodies, not fixed game RVAs.
    const auto update = static_cast<const unsigned char*>(g_UpdateEntry);
    const auto cursor = reinterpret_cast<const unsigned char*>(g_Cursor);
    g_ManagerSlot = reinterpret_cast<void**>(const_cast<unsigned char*>(update + 0x79 +
        *reinterpret_cast<const int*>(update + 0x75)));
    g_UiManagerOffset = *reinterpret_cast<const unsigned*>(update + 0x7C);
    g_InputBlockedOffset = *reinterpret_cast<const unsigned*>(update + 0x19);
    // Verified from the original transaction body's loading of the input manager.
    if (nativeManager[0x4D6] != 0x48 || nativeManager[0x4D7] != 0x8B ||
        nativeManager[0x4D8] != 0x80 ||
        Relative(nativeManager + 0x40) != reinterpret_cast<const unsigned char*>(g_ManagerSlot)) {
        Log("Hot switch input manager pointer cross-check failed; feature disabled");
        g_Enabled.store(false);
        return;
    }
    g_InputManagerOffset = *reinterpret_cast<const unsigned*>(nativeManager + 0x4D9);
    g_InputModeOffset = *reinterpret_cast<const unsigned*>(reinterpret_cast<const unsigned char*>(g_SetInputMode) + 0x1E);
    g_UiModeOffset = *reinterpret_cast<const unsigned*>(reinterpret_cast<const unsigned char*>(g_SetUiMode) + 0x2E);
    const auto uiSetter = reinterpret_cast<const unsigned char*>(g_SetUiMode);
    // Validate the native wrapper against all three already matched entries.
    // Its generic HUD getter requires its real MethodInfo, not a null pointer.
    g_GetHud = reinterpret_cast<GetPage>(const_cast<unsigned char*>(CallTarget(sequence + 0x07)));
    g_GetUiKind = reinterpret_cast<GetKind>(const_cast<unsigned char*>(CallTarget(sequence + 0x73)));
    g_RefreshHud = reinterpret_cast<RefreshHud>(const_cast<unsigned char*>(CallTarget(sequence + 0x8A)));
    g_HudMethodSlot = reinterpret_cast<void**>(const_cast<unsigned char*>(Relative(sequence + 0x03)));
    // Independently verify that the second 7.2 wrapper points to these same
    // dependencies, so unrelated page wrappers cannot satisfy the scan.
    if (CallTarget(otherSequence + 0x07) != reinterpret_cast<const unsigned char*>(g_GetHud) ||
        CallTarget(otherSequence + 0x73) != reinterpret_cast<const unsigned char*>(g_GetUiKind) ||
        CallTarget(otherSequence + 0x8A) != reinterpret_cast<const unsigned char*>(g_RefreshHud) ||
        Relative(otherSequence + 0x03) != reinterpret_cast<const unsigned char*>(g_HudMethodSlot) ||
        CallTarget(otherSequence + 0x2E) != reinterpret_cast<const unsigned char*>(g_SetUiMode) ||
        CallTarget(otherSequence + 0x52) != reinterpret_cast<const unsigned char*>(g_SetInputMode) ||
        CallTarget(otherSequence + 0xA6) != cursor ||
        Relative(otherSequence + 0x12) != reinterpret_cast<const unsigned char*>(g_ManagerSlot) ||
        *reinterpret_cast<const unsigned*>(otherSequence + 0x19) != g_UiManagerOffset) {
        Log("Hot switch 7.2 wrappers disagree; feature disabled");
        g_Enabled.store(false);
        return;
    }
    // The cursor body independently references the same root and UI slot.
    const auto cursorSlot = cursor + 0x2F + *reinterpret_cast<const int*>(cursor + 0x2B);
    const auto cursorUi = *reinterpret_cast<const unsigned*>(cursor + 0x32);
    if (cursorSlot != reinterpret_cast<const unsigned char*>(g_ManagerSlot) || cursorUi != g_UiManagerOffset ||
        !g_InputBlockedOffset || g_InputBlockedOffset >= 0x1000 ||
        !g_InputManagerOffset || g_InputManagerOffset >= 0x40000 ||
        (g_InputManagerOffset % sizeof(void*)) ||
        g_UiModeOffset != build.uiModeOffset || g_InputModeOffset != build.inputModeOffset ||
        g_InputModeOffset != *reinterpret_cast<const unsigned*>(cursor + 0x16) ||
        g_InputModeOffset != *reinterpret_cast<const unsigned*>(update + 0x4D) ||
        CallTarget(sequence + 0x2E) != uiSetter ||
        CallTarget(sequence + 0x52) != reinterpret_cast<const unsigned char*>(g_SetInputMode) ||
        CallTarget(sequence + 0xA6) != cursor ||
        Relative(sequence + 0x12) != reinterpret_cast<const unsigned char*>(g_ManagerSlot) ||
        *reinterpret_cast<const unsigned*>(sequence + 0x19) != g_UiManagerOffset ||
        !g_GetHud || !g_GetUiKind || !g_RefreshHud ||
        CallTarget(uiSetter + 0x58) != reinterpret_cast<const unsigned char*>(g_GetUiKind) ||
        uiSetter[0x5D] != 0x89 || uiSetter[0x5E] != 0x86 ||
        uiSetter[0xC7] != 0xFF || uiSetter[0xC8] != 0x90) {
        Log("Hot switch native field cross-check failed; feature disabled");
        g_Enabled.store(false);
        return;
    }
    const auto layout = CallTarget(uiSetter + 0x50);
    if (!layout || layout[0x88] != 0x48 || layout[0x89] != 0x89 || layout[0x8A] != 0xBE) {
        Log("Hot switch UI layout body check failed; feature disabled");
        g_Enabled.store(false);
        return;
    }
    g_UiKindOffset = *reinterpret_cast<const unsigned*>(uiSetter + 0x5F);
    g_PagesOffset = MemberLoadOffset(uiSetter + 0x6B, 0x06); // rax, [rsi+offset]
    g_PageRefreshOffset = *reinterpret_cast<const unsigned*>(uiSetter + 0xC9);
    g_ReferenceOffset = *reinterpret_cast<const unsigned*>(layout + 0x8B);
    const auto begin = reinterpret_cast<const unsigned char*>(g_BeginMenus);
    const auto finish = reinterpret_cast<const unsigned char*>(g_FinishMenus);
    const auto preparePage = CallTarget(begin + 0x8F);
    if (!preparePage || !CallTarget(begin + 0x4A) || CallTarget(begin + 0x4A) != CallTarget(finish + 0x4A) ||
        !g_PagesOffset || MemberLoadOffset(begin + 0xF8, 0x0E) != g_PagesOffset ||
        MemberLoadOffset(finish + 0x112, 0x0B) != g_PagesOffset ||
        Relative(preparePage + 0x2B) != reinterpret_cast<const unsigned char*>(g_ManagerSlot) ||
        *reinterpret_cast<const unsigned*>(preparePage + 0x32) != g_UiManagerOffset ||
        *reinterpret_cast<const unsigned*>(preparePage + 0x42) != g_InputManagerOffset ||
        *reinterpret_cast<const unsigned*>(preparePage + 0x51) != g_UiModeOffset ||
        *reinterpret_cast<const unsigned*>(preparePage + 0x57) != g_InputModeOffset ||
        preparePage[0x1F] != 0x83 || preparePage[0x20] != 0xBF ||
        preparePage[0x63] != 0x80 || preparePage[0x64] != 0xBF ||
        preparePage[0x6A] != 0x48 || preparePage[0x6B] != 0x8B || preparePage[0x6C] != 0x47 ||
        preparePage[0x92] != 0x89 || preparePage[0x93] != 0x68 ||
        preparePage[0x95] != 0x89 || preparePage[0x96] != 0x70 ||
        preparePage[0x9F] != 0x89 || preparePage[0xA0] != 0x58 ||
        preparePage[0xA2] != 0x44 || preparePage[0xA3] != 0x89 || preparePage[0xA4] != 0x70) {
        Log("Hot switch native menu transition cross-check failed; feature disabled");
        g_Enabled.store(false);
        return;
    }
    // Native stack count and backing array, read by the HUD guard and diagnostics.
    g_PageCountOffset = overseas ? 0x24 : 0x20;
    g_PageItemsOffset = overseas ? 0x10 : 0x18;
    g_PageReadyOffset = *reinterpret_cast<const unsigned*>(preparePage + 0x21);
    g_PageDirtyOffset = *reinterpret_cast<const unsigned*>(preparePage + 0x65);
    g_PageModeStateOffset = preparePage[0x6D];
    g_PreviousUiOffset = preparePage[0x94]; g_PreviousInputOffset = preparePage[0x97];
    g_TargetUiOffset = preparePage[0xA1]; g_TargetInputOffset = preparePage[0xA5];
    PrepareWorldCursorFix(build, nativeManager, begin);
    // The input manager is the receiver supplied by its own native update;
    // no additional root-table offset or setting-page instance is guessed.
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    char text[256]{};
    sprintf_s(text, "Hot switch entries resolved once; update=0x%llX; UI setter=0x%llX; input setter=0x%llX; cursor=0x%llX",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_UpdateEntry) - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_SetUiMode) - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_SetInputMode) - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_Cursor) - base));
    Log(text);
    sprintf_s(text, "Hot switch native menu transition resolved; begin=0x%llX; finish=0x%llX; device changes only",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_BeginMenus) - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_FinishMenus) - base));
    Log(text);
    sprintf_s(text, "Hot switch native HUD chain resolved; getter=0x%llX; UI kind=0x%llX; HUD refresh=0x%llX",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_GetHud) - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_GetUiKind) - base),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_RefreshHud) - base));
    Log(text);
}
void HotSwitch::Install() {
    if (!IsEnabled() || !g_Prepared || g_InstallAttempted) return;
    g_InstallAttempted = true;
    if (MH_CreateHook(g_UpdateEntry, reinterpret_cast<void*>(&HookUpdate),
        reinterpret_cast<void**>(&g_OriginalUpdate)) != MH_OK || MH_EnableHook(g_UpdateEntry) != MH_OK) {
        if (g_OriginalUpdate) MH_RemoveHook(g_UpdateEntry);
        Log("Hot switch UI-update hook failed; feature disabled");
        g_Enabled.store(false);
        return;
    }
    g_Ready.store(true, std::memory_order_release);
    Log("7.2 native menu transition ready; begin/UI/input/HUD/cursor/finish");
}
