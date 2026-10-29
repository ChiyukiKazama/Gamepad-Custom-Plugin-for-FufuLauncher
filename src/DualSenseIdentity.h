#pragma once

// A DualSense Edge reports its own product ID (054C:0DF2) and product name.
// Software that selects its DualSense support by that identity ignores the Edge
// completely, so the DualSense features of that path (gyro calibration, haptics
// and trigger effects) are never initialized even though the device speaks the
// same HID reports. This adapter presents the Edge as a standard DualSense
// (054C:0CE6) inside the current process only: identity queries see the standard
// product, while handles, report contents and output effects keep reaching the
// physical device unchanged.
namespace DualSenseIdentity {
    using Logger = void (*)(const char*);
    // enabled: present the Edge as a standard DualSense. Standard DualSense,
    // DualShock 4 and every other device keep their own identity.
    void Configure(Logger log, bool enabled);
    // Safe to call repeatedly. Installs the identity view on hid.dll once it is
    // loaded; call it after DualSenseHidBackend so that adapter keeps the real
    // hid.dll exports it captured.
    void Install();
}
