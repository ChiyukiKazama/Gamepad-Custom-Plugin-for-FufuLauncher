#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <hidsdi.h>
#include <cstring>
#include "MinHook.h"
#include "DualSenseIdentity.h"

namespace {
    using GetAttributes = decltype(&HidD_GetAttributes);
    using GetString = decltype(&HidD_GetProductString);
    constexpr USHORT SonyVendor = 0x054C;
    constexpr USHORT DualSenseProduct = 0x0CE6;
    constexpr USHORT DualSenseEdgeProduct = 0x0DF2;
    constexpr wchar_t DualSenseProductName[] = L"DualSense Wireless Controller";
    GetAttributes g_OriginalAttributes = nullptr;
    GetString g_OriginalProduct = nullptr;
    DualSenseIdentity::Logger g_Log = nullptr;
    bool g_Enabled = false, g_Attempted = false;

    void Log(const char* text) { if (g_Log) g_Log(text); }

    // Reads the driver's own identity through the trampoline, so this never
    // observes the presented value it is about to produce.
    bool IsEdge(HANDLE device) {
        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        return g_OriginalAttributes && g_OriginalAttributes(device, &attributes) &&
            attributes.VendorID == SonyVendor && attributes.ProductID == DualSenseEdgeProduct;
    }

    BOOLEAN WINAPI HookAttributes(HANDLE device, HIDD_ATTRIBUTES* attributes) {
        const BOOLEAN result = g_OriginalAttributes(device, attributes);
        // Only the Edge identity is presented as a standard DualSense; every
        // other device keeps the values the driver reported.
        if (result && attributes && attributes->VendorID == SonyVendor &&
            attributes->ProductID == DualSenseEdgeProduct) attributes->ProductID = DualSenseProduct;
        return result;
    }

    BOOLEAN WINAPI HookProductString(HANDLE device, PVOID buffer, ULONG length) {
        const BOOLEAN result = g_OriginalProduct(device, buffer, length);
        // The caller's buffer is reused only when the standard name fits, and
        // the tail stays zero-terminated like the original string.
        const unsigned capacity = length / sizeof(wchar_t);
        if (!result || !buffer || capacity < ARRAYSIZE(DualSenseProductName)) return result;
        if (!IsEdge(device)) return result;
        std::memcpy(buffer, DualSenseProductName, sizeof(DualSenseProductName));
        std::memset(static_cast<wchar_t*>(buffer) + ARRAYSIZE(DualSenseProductName), 0,
            length - sizeof(DualSenseProductName));
        return result;
    }
}

void DualSenseIdentity::Configure(Logger log, bool enabled) {
    g_Log = log;
    g_Enabled = enabled;
}

void DualSenseIdentity::Install() {
    if (!g_Enabled || g_Attempted) return;
    const auto hid = GetModuleHandleW(L"hid.dll");
    if (!hid) return;
    g_Attempted = true;
    void* targets[] = {
        reinterpret_cast<void*>(GetProcAddress(hid, "HidD_GetAttributes")),
        reinterpret_cast<void*>(GetProcAddress(hid, "HidD_GetProductString"))
    };
    void* detours[] = {
        reinterpret_cast<void*>(&HookAttributes), reinterpret_cast<void*>(&HookProductString)
    };
    void** originals[] = {
        reinterpret_cast<void**>(&g_OriginalAttributes), reinterpret_cast<void**>(&g_OriginalProduct)
    };
    unsigned created = 0;
    bool ready = true;
    for (unsigned index = 0; ready && index < ARRAYSIZE(targets); ++index) {
        ready = targets[index] && MH_CreateHook(targets[index], detours[index], originals[index]) == MH_OK;
        if (ready) ++created;
    }
    for (unsigned index = 0; ready && index < ARRAYSIZE(targets); ++index)
        ready = MH_EnableHook(targets[index]) == MH_OK;
    if (!ready) {
        for (unsigned index = 0; index < created; ++index) {
            MH_DisableHook(targets[index]);
            MH_RemoveHook(targets[index]);
        }
        Log("DualSense Edge identity view unavailable; device identity unchanged");
        return;
    }
    Log("DualSense Edge presented as a standard DualSense for this process; device reports are unchanged");
}
