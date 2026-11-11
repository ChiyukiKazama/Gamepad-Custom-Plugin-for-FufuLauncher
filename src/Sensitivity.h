#pragma once
namespace GameBuild { struct Profile; }
namespace Sensitivity {
    using Logger = void (*)(const char*);
    using Resolver = void* (*)(const char*);
    void Configure(Logger log, float factor, bool diagnostics);
    void SetFactor(float factor); // Atomic live parameter; does not install a hook.
    void Prepare(Resolver find, const GameBuild::Profile& build);
    void Install();
}
