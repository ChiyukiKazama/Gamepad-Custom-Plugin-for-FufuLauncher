#pragma once

namespace GameBuild { struct Profile; }
namespace GamepadLayout {
    using Logger = void (*)(const char*);
    void Install(Logger log, const GameBuild::Profile& build);
}
