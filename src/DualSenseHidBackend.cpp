#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <hidsdi.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "MinHook.h"
#include "DualSenseHidBackend.h"
#include "DualSenseHidState.h"

namespace {
    using GetData = decltype(&HidP_GetData);
    using GetCaps = decltype(&HidP_GetCaps);
    using GetButtons = decltype(&HidP_GetButtonCaps);
    using GetValues = decltype(&HidP_GetValueCaps);
    using GetPreparsed = decltype(&HidD_GetPreparsedData);
    using FreePreparsed = decltype(&HidD_FreePreparsedData);
    using GetAttributes = decltype(&HidD_GetAttributes);
    using GetRawInfo = decltype(&GetRawInputDeviceInfoW);
    using GetRawData = decltype(&GetRawInputData);
    using SetData = decltype(&HidP_SetData);
    GetData g_OriginalData = nullptr;
    GetPreparsed g_OriginalPreparsed = nullptr;
    FreePreparsed g_OriginalFree = nullptr;
    GetRawInfo g_OriginalRawInfo = nullptr;
    GetRawData g_OriginalRawData = nullptr;
    SetData g_SetData = nullptr;
    GetCaps g_GetCaps = nullptr;
    GetButtons g_GetButtons = nullptr;
    GetValues g_GetValues = nullptr;
    GetAttributes g_GetAttributes = nullptr;
    DualSenseHidBackend::Logger g_Log = nullptr;
    DualSenseHidBackend::Wanted g_Wanted = nullptr;
    DualSenseHidBackend::Observed g_Observed = nullptr;
    DualSenseHidBackend::Process g_Process = nullptr;
    DualSenseHidBackend::Disconnect g_Disconnect = nullptr;
    struct Device {
        std::array<DualSenseHidLayout, 2> reports;
        HANDLE handle = nullptr;
        bool rawInput = false;
        bool rawObserved = false;
        std::vector<unsigned char> descriptor;
    };
    std::mutex g_Mutex;
    // Raw Input may copy the same descriptor repeatedly, even to a new buffer.
    // Keep one layout and routing identity per device, independent of that buffer.
    std::unordered_map<PHIDP_PREPARSED_DATA, std::shared_ptr<Device>> g_Devices;
    using DeviceCache = std::unordered_map<HANDLE, std::weak_ptr<Device>>;
    DeviceCache g_RawDevices, g_HidDevices;
    std::unordered_map<HANDLE, std::shared_ptr<Device>> g_RawOwners;
    std::unordered_set<HANDLE> g_UnsupportedRaw;
    std::atomic<unsigned> g_Traced{0};
    bool g_Attempted = false;
    void Log(const char* text) { if (g_Log) g_Log(text); }
    void LogOnce(unsigned flag, const char* text) {
        if (!(g_Traced.load(std::memory_order_relaxed) & flag) &&
            !(g_Traced.fetch_or(flag, std::memory_order_relaxed) & flag)) Log(text);
    }
    // The Edge shares the DualSense report layout, so both product IDs use the
    // same adapter. Identity presentation to the game lives in DualSenseIdentity.
    bool IsDualSense(unsigned vendor, unsigned product) {
        return vendor == 0x054C && (product == 0x0CE6 || product == 0x0DF2);
    }
    int ReportSlot(UCHAR id) { return id == 1 ? 0 : id == 0x31 ? 1 : -1; }

    bool BuildDevice(PHIDP_PREPARSED_DATA preparsed, Device& device) {
        HIDP_CAPS caps{};
        if (g_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS || caps.UsagePage != 1 || caps.Usage != 5 ||
            caps.NumberInputButtonCaps > 64 || caps.NumberInputValueCaps > 128) return false;
        USHORT buttonCount = caps.NumberInputButtonCaps;
        std::vector<HIDP_BUTTON_CAPS> buttons(buttonCount);
        if (!buttonCount || g_GetButtons(HidP_Input, buttons.data(), &buttonCount, preparsed) != HIDP_STATUS_SUCCESS)
            return false;
        for (USHORT i = 0; i < buttonCount; ++i) {
            const auto& cap = buttons[i];
            const int slot = ReportSlot(cap.ReportID);
            if (slot < 0 || cap.UsagePage != 9) continue;
            auto& layout = device.reports[slot];
            const USHORT first = cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage;
            const USHORT last = cap.IsRange ? cap.Range.UsageMax : first;
            const USHORT index = cap.IsRange ? cap.Range.DataIndexMin : cap.NotRange.DataIndex;
            if (first < 1 || last > 15 || last < first) continue;
            for (unsigned usage = first; usage <= last; ++usage) {
                const unsigned dataIndex = index + usage - first;
                if (dataIndex >= layout.fields.size()) return false;
                layout.fields[dataIndex] = {DualSenseHidLayout::Kind::Button, usage - 1};
                layout.buttons[usage - 1] = static_cast<USHORT>(dataIndex);
                layout.buttonMask |= 1u << (usage - 1);
            }
        }
        USHORT valueCount = caps.NumberInputValueCaps;
        std::vector<HIDP_VALUE_CAPS> values(valueCount);
        if (!valueCount || g_GetValues(HidP_Input, values.data(), &valueCount, preparsed) != HIDP_STATUS_SUCCESS)
            return false;
        for (USHORT i = 0; i < valueCount; ++i) {
            const auto& cap = values[i];
            const int slot = ReportSlot(cap.ReportID);
            if (slot < 0 || cap.UsagePage != 1 || cap.ReportCount != 1 || cap.IsRange) continue;
            auto& layout = device.reports[slot];
            const unsigned index = cap.NotRange.DataIndex;
            if (index >= layout.fields.size()) return false;
            const USHORT usage = cap.NotRange.Usage;
            if (usage == 0x39 && cap.LogicalMin == 0 && cap.LogicalMax <= 15) {
                layout.fields[index] = {DualSenseHidLayout::Kind::Hat, 0};
                layout.hat = true;
            } else if (cap.LogicalMin == 0 && cap.LogicalMax == 255) {
                // Sony HID: X/Y = left stick, Z/Rz = right stick, Rx/Ry = triggers.
                const int axis = usage == 0x30 ? 0 : usage == 0x31 ? 1 : usage == 0x32 ? 2 :
                    usage == 0x35 ? 3 : usage == 0x33 ? 4 : usage == 0x34 ? 5 : -1;
                if (axis < 0) continue;
                layout.fields[index] = {DualSenseHidLayout::Kind::Axis, static_cast<unsigned>(axis)};
                layout.axisMask |= 1u << axis;
            }
        }
        return device.reports[0].Ready() || device.reports[1].Ready();
    }

    // g_Mutex is held. Weak device caches do not keep an unused HID allocation alive.
    void ForgetDevice(PHIDP_PREPARSED_DATA preparsed) {
        const auto found = g_Devices.find(preparsed);
        if (found == g_Devices.end()) return;
        auto device = found->second;
        g_Devices.erase(found);
        if (device.use_count() == 1) {
            (device->rawInput ? g_RawDevices : g_HidDevices).erase(device->handle);
            if (g_Disconnect) g_Disconnect(device.get());
        }
    }
    bool KnownDevice(PHIDP_PREPARSED_DATA preparsed, HANDLE handle, bool rawInput) {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const auto found = g_Devices.find(preparsed);
        return found != g_Devices.end() && found->second->handle == handle &&
            found->second->rawInput == rawInput;
    }
    void RegisterDevice(PHIDP_PREPARSED_DATA preparsed, HANDLE handle, bool rawInput, bool supported, UINT descriptorBytes = 0) {
        if (!preparsed) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (!supported) { ForgetDevice(preparsed); return; }
        if (rawInput) g_UnsupportedRaw.erase(handle);
        auto& cache = rawInput ? g_RawDevices : g_HidDevices;
        const auto cached = cache.find(handle);
        auto device = cached != cache.end() ? cached->second.lock() : nullptr;
        if (!device) {
            device = std::make_shared<Device>();
            if (!BuildDevice(preparsed, *device)) {
                ForgetDevice(preparsed);
                LogOnce(2, "DualSense Windows HID capabilities unsupported; decoded input unchanged");
                return;
            }
            device->handle = handle;
            device->rawInput = rawInput;
            cache[handle] = device;
            LogOnce(1, "DualSense Windows HID capabilities registered; decoded-input adapter ready");
        }
        if (rawInput && descriptorBytes && descriptorBytes <= 65536 && device->descriptor.empty()) {
            const auto bytes = reinterpret_cast<const unsigned char*>(preparsed);
            device->descriptor.assign(bytes, bytes + descriptorBytes);
        }
        const auto found = g_Devices.find(preparsed);
        if (found != g_Devices.end() && found->second == device) return;
        ForgetDevice(preparsed);
        g_Devices.emplace(preparsed, std::move(device));
    }

    UINT WINAPI HookRawInfo(HANDLE handle, UINT command, LPVOID data, PUINT size) {
        const UINT result = g_OriginalRawInfo(handle, command, data, size);
        if (command == RIDI_PREPARSEDDATA && result != static_cast<UINT>(-1) && data && result) {
            const auto preparsed = static_cast<PHIDP_PREPARSED_DATA>(data);
            if (KnownDevice(preparsed, handle, true)) return result;
            RID_DEVICE_INFO info{};
            info.cbSize = sizeof(info);
            UINT bytes = sizeof(info);
            const bool supported = g_OriginalRawInfo(handle, RIDI_DEVICEINFO, &info, &bytes) != static_cast<UINT>(-1) &&
                info.dwType == RIM_TYPEHID && IsDualSense(info.hid.dwVendorId, info.hid.dwProductId);
            RegisterDevice(preparsed, handle, true, supported, result);
        }
        return result;
    }
    BOOLEAN WINAPI HookPreparsed(HANDLE handle, PHIDP_PREPARSED_DATA* data) {
        const BOOLEAN result = g_OriginalPreparsed(handle, data);
        if (result && data && *data) {
            if (KnownDevice(*data, handle, false)) return result;
            HIDD_ATTRIBUTES attributes{};
            attributes.Size = sizeof(attributes);
            RegisterDevice(*data, handle, false, g_GetAttributes(handle, &attributes) &&
                IsDualSense(attributes.VendorID, attributes.ProductID));
        }
        return result;
    }
    BOOLEAN WINAPI HookFree(PHIDP_PREPARSED_DATA data) {
        const BOOLEAN result = g_OriginalFree(data);
        if (result) {
            std::lock_guard<std::mutex> lock(g_Mutex);
            ForgetDevice(data);
        }
        return result;
    }

    // g_Mutex held. Raw Input owns the device handle, not its temporary copied
    // descriptor buffers. Inspect stale handles only when a new PS5 is observed.
    std::shared_ptr<Device> RawDevice(HANDLE handle) {
        const auto owned = g_RawOwners.find(handle);
        if (owned != g_RawOwners.end()) return owned->second;
        if (g_UnsupportedRaw.count(handle)) return nullptr;
        RID_DEVICE_INFO info{}; info.cbSize = sizeof(info);
        UINT bytes = sizeof(info);
        if (g_OriginalRawInfo(handle, RIDI_DEVICEINFO, &info, &bytes) == static_cast<UINT>(-1) ||
            info.dwType != RIM_TYPEHID || !IsDualSense(info.hid.dwVendorId, info.hid.dwProductId)) {
            g_UnsupportedRaw.insert(handle);
            return nullptr;
        }
        for (auto it = g_RawOwners.begin(); it != g_RawOwners.end();) {
            RID_DEVICE_INFO current{}; current.cbSize = sizeof(current); bytes = sizeof(current);
            if (g_OriginalRawInfo(it->first, RIDI_DEVICEINFO, &current, &bytes) != static_cast<UINT>(-1)) {
                ++it; continue;
            }
            auto device = it->second;
            for (auto alias = g_Devices.begin(); alias != g_Devices.end();) {
                if (alias->second == device) alias = g_Devices.erase(alias); else ++alias;
            }
            g_RawDevices.erase(it->first);
            if (g_Disconnect) g_Disconnect(device.get());
            it = g_RawOwners.erase(it);
        }
        const auto cached = g_RawDevices.find(handle);
        auto device = cached != g_RawDevices.end() ? cached->second.lock() : nullptr;
        if (!device || device->descriptor.empty()) {
            bytes = 0;
            if (g_OriginalRawInfo(handle, RIDI_PREPARSEDDATA, nullptr, &bytes) == static_cast<UINT>(-1) ||
                !bytes || bytes > 65536) return nullptr;
            std::vector<unsigned char> descriptor(bytes);
            if (g_OriginalRawInfo(handle, RIDI_PREPARSEDDATA, descriptor.data(), &bytes) == static_cast<UINT>(-1))
                return nullptr;
            auto candidate = std::make_shared<Device>();
            if (!BuildDevice(reinterpret_cast<PHIDP_PREPARSED_DATA>(descriptor.data()), *candidate)) return nullptr;
            if (!device) device = candidate;
            device->descriptor = std::move(descriptor);
            device->handle = handle; device->rawInput = true;
            g_RawDevices[handle] = device;
        }
        g_RawOwners[handle] = device;
        return device;
    }

    UINT WINAPI HookRawData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT headerSize) {
        const UINT result = g_OriginalRawData(input, command, data, size, headerSize);
        constexpr UINT payloadOffset = offsetof(RAWINPUT, data.hid.bRawData);
        if (command != RID_INPUT || result == static_cast<UINT>(-1) || !data || result < payloadOffset ||
            headerSize != sizeof(RAWINPUTHEADER)) return result;
        auto& raw = *static_cast<RAWINPUT*>(data);
        if (raw.header.dwType != RIM_TYPEHID || raw.header.dwSize > result ||
            raw.header.dwSize < payloadOffset || raw.data.hid.dwSizeHid != 64 || !raw.data.hid.dwCount ||
            raw.data.hid.dwCount > (raw.header.dwSize - payloadOffset) / raw.data.hid.dwSizeHid) return result;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const auto device = RawDevice(raw.header.hDevice);
        if (!device || device->descriptor.empty() || !device->reports[0].Ready()) return result;
        const auto preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(device->descriptor.data());
        for (DWORD i = 0; i < raw.data.hid.dwCount; ++i) {
            auto report = reinterpret_cast<char*>(raw.data.hid.bRawData + 64 * i);
            if (static_cast<UCHAR>(report[0]) != 1) continue; // USB only; no Bluetooth CRC rewriting.
            std::array<HIDP_DATA, DualSenseHidState::MaximumData> decoded{};
            ULONG count = static_cast<ULONG>(decoded.size());
            if (g_OriginalData(HidP_Input, decoded.data(), &count, preparsed, report, 64) != HIDP_STATUS_SUCCESS)
                continue;
            DualSenseHidState snapshot;
            if (!snapshot.Read(decoded.data(), count, device->reports[0])) continue;
            device->rawObserved = true;
            if (g_Observed) g_Observed();
            LogOnce(32, "Game reads PS5 USB Raw Input; remapping the game-owned report copy");
            if (!g_Wanted || !g_Wanted()) continue;
            auto pad = snapshot.state.ToGamepad();
            if (!g_Process || !g_Process(device.get(), pad)) continue;
            std::array<HIDP_DATA, DualSenseHidState::MaximumData> changes{};
            ULONG changed = snapshot.ReportChanges(changes.data(), device->reports[0], pad);
            if (!changed) continue;
            std::array<char, 64> mapped;
            std::memcpy(mapped.data(), report, mapped.size());
            if (g_SetData(HidP_Input, changes.data(), &changed, preparsed, mapped.data(),
                static_cast<ULONG>(mapped.size())) == HIDP_STATUS_SUCCESS) {
                std::memcpy(report, mapped.data(), mapped.size());
                LogOnce(64, "PS5 USB raw-copy carrier applied; decoded-data routing disabled for this device");
            } else LogOnce(128, "PS5 USB raw-copy field rewrite unavailable; copied report unchanged");
        }
        return result;
    }
    NTSTATUS WINAPI HookData(HIDP_REPORT_TYPE type, PHIDP_DATA data, PULONG count,
        PHIDP_PREPARSED_DATA preparsed, PCHAR report, ULONG reportLength) {
        const ULONG capacity = count ? *count : 0;
        const NTSTATUS result = g_OriginalData(type, data, count, preparsed, report, reportLength);
        if (result != HIDP_STATUS_SUCCESS || type != HidP_Input || !data || !count || !report || !reportLength)
            return result;
        const int slot = ReportSlot(static_cast<UCHAR>(report[0]));
        if (slot < 0) return result;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const auto found = g_Devices.find(preparsed);
        if (found == g_Devices.end()) return result;
        auto& device = *found->second;
        // Sensitivity initialization also needs to observe input with map routing disabled.
        if (g_Observed) g_Observed();
        if (device.rawObserved) return result; // The same report must not reset or re-arm its raw-layer routing state.
        if (!g_Wanted || !g_Wanted()) return result;
        DualSenseHidState snapshot;
        if (!snapshot.Read(data, *count, device.reports[slot])) {
            LogOnce(4, "DualSense Windows HID decoded state incomplete; input unchanged");
            return result;
        }
        LogOnce(8, "Game reads DualSense through Windows HID decoded data");
        auto pad = snapshot.state.ToGamepad();
        if (g_Process && g_Process(&device, pad) &&
            !snapshot.WriteChanges(data, *count, capacity, device.reports[slot], pad)) {
            LogOnce(16, "DualSense Windows HID carrier could not fit; decoded input unchanged");
        }
        return result;
    }
}

void DualSenseHidBackend::Configure(Logger log, Wanted wanted, Observed observed, Process process, Disconnect disconnect) {
    g_Log = log; g_Wanted = wanted; g_Observed = observed; g_Process = process; g_Disconnect = disconnect;
}
void DualSenseHidBackend::Install() {
    if (g_Attempted) return;
    const auto hid = GetModuleHandleW(L"hid.dll");
    const auto user = GetModuleHandleW(L"user32.dll");
    if (!hid || !user) return;
    g_Attempted = true;
    g_GetCaps = reinterpret_cast<GetCaps>(GetProcAddress(hid, "HidP_GetCaps"));
    g_GetButtons = reinterpret_cast<GetButtons>(GetProcAddress(hid, "HidP_GetButtonCaps"));
    g_GetValues = reinterpret_cast<GetValues>(GetProcAddress(hid, "HidP_GetValueCaps"));
    g_GetAttributes = reinterpret_cast<GetAttributes>(GetProcAddress(hid, "HidD_GetAttributes"));
    g_SetData = reinterpret_cast<SetData>(GetProcAddress(hid, "HidP_SetData"));
    void* targets[] = {
        reinterpret_cast<void*>(GetProcAddress(user, "GetRawInputDeviceInfoW")),
        reinterpret_cast<void*>(GetProcAddress(hid, "HidD_GetPreparsedData")),
        reinterpret_cast<void*>(GetProcAddress(hid, "HidD_FreePreparsedData")),
        reinterpret_cast<void*>(GetProcAddress(hid, "HidP_GetData")),
        reinterpret_cast<void*>(GetProcAddress(user, "GetRawInputData"))
    };
    void* detours[] = {reinterpret_cast<void*>(&HookRawInfo), reinterpret_cast<void*>(&HookPreparsed),
        reinterpret_cast<void*>(&HookFree), reinterpret_cast<void*>(&HookData), reinterpret_cast<void*>(&HookRawData)};
    void** originals[] = {reinterpret_cast<void**>(&g_OriginalRawInfo), reinterpret_cast<void**>(&g_OriginalPreparsed),
        reinterpret_cast<void**>(&g_OriginalFree), reinterpret_cast<void**>(&g_OriginalData),
        reinterpret_cast<void**>(&g_OriginalRawData)};
    unsigned created = 0;
    bool ready = g_GetCaps && g_GetButtons && g_GetValues && g_GetAttributes && g_SetData;
    for (unsigned i = 0; ready && i < ARRAYSIZE(targets); ++i) {
        ready = targets[i] && MH_CreateHook(targets[i], detours[i], originals[i]) == MH_OK;
        if (ready) ++created;
    }
    for (unsigned i = 0; ready && i < ARRAYSIZE(targets); ++i) ready = MH_EnableHook(targets[i]) == MH_OK;
    if (!ready) {
        for (unsigned i = 0; i < created; ++i) { MH_DisableHook(targets[i]); MH_RemoveHook(targets[i]); }
        Log("DualSense Windows HID hooks unavailable; existing input backends retained");
        return;
    }
    Log("DualSense Windows HID hooks installed; PS5 USB remaps game-owned input copies");
}
