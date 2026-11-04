#pragma once
#include <Windows.h>
#include <cstdint>

// CN 7.2, verified against cn72.exe. Native object writes are only valid for
// this build; a launcher version label alone cannot select a memory layout.
namespace GameBuild {
    inline constexpr std::uint32_t Timestamp = 0x6AD886FB;
    inline constexpr std::uint32_t ImageSize = 0x1B65F000;
    inline constexpr std::uintptr_t SetActiveRva = 0x1463FC0;

    inline bool SupportedImage(const void* image) {
        if (!image) return false;
        const auto bytes = static_cast<const unsigned char*>(image);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) ||
            dos->e_lfanew > 0x1000) return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
            nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
            nt->FileHeader.TimeDateStamp == Timestamp && nt->OptionalHeader.SizeOfImage == ImageSize;
    }
}
