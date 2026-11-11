#pragma once
#include <Windows.h>
#include <cstdint>

// Verified against cn72.exe and os72.exe. A launcher version label alone
// cannot select a native memory layout.
namespace GameBuild {
    enum class Region { CN, OS };
    struct Profile {
        Region region;
        const wchar_t* executable;
        std::uint32_t timestamp, imageSize;
        std::uintptr_t setActiveRva;
        unsigned uiModeOffset, inputModeOffset;
    };
    inline constexpr Profile CN72{ Region::CN, L"YuanShen.exe", 0x6AD886FB, 0x1B65F000, 0x1463FC0, 0x324, 0x1AC };
    inline constexpr Profile OS72{ Region::OS, L"GenshinImpact.exe", 0x6AD8992C, 0x1B690000, 0x1464FC0, 0x370, 0x158 };

    inline const Profile* Match(const wchar_t* executable, const void* image) {
        const Profile* build = !_wcsicmp(executable, CN72.executable) ? &CN72 :
            !_wcsicmp(executable, OS72.executable) ? &OS72 : nullptr;
        if (!build || !image) return nullptr;
        const auto bytes = static_cast<const unsigned char*>(image);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) ||
            dos->e_lfanew > 0x1000) return nullptr;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
            nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
            nt->FileHeader.TimeDateStamp == build->timestamp && nt->OptionalHeader.SizeOfImage == build->imageSize
            ? build : nullptr;
    }
}
