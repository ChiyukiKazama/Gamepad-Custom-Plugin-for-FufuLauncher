#pragma once
namespace GameBuild { struct Profile; }
namespace NativePages {
    using Logger = void (*)(const char*);
    void Configure(Logger log, bool mapBinding, bool diagnostics, bool hotSwitch, bool sensitivity,
        const wchar_t* pluginDirectory, const GameBuild::Profile& build);
    void* FindNative(const char* pattern); // Worker-only unique-match resolver.
    bool MatchesMainPath(const wchar_t* path, unsigned characters);
    void Prepare(); // Once, after the existing main-plugin module is loaded.
    bool IsReady(); // Main-plugin-dependent initialization completed.
    bool RequestActivation(); // Signals the worker only once when main hooks are active.
    void Install(); // Worker only; never installs hooks inside XInput or a loader callback.
    bool RequestMap(bool enabled); // Arms one native action conversion; false leaves input unchanged.
    void CancelMapRequest();
}
