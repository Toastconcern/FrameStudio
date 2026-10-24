// SteamVR's own OpenVR client library, loaded at run time from wherever SteamVR is installed on this PC: openvr.h's
// entry points, forwarded to it. A PC build of Frame Home and its panel then runs on any PC with SteamVR (Windows or
// Linux) without carrying a copy, the way the Steam Frame build links SteamVR's own (built with OPENVR_BUILD_STATIC,
// so openvr.h declares these as plain functions for this file to define).
//
// Where SteamVR is: OpenVR's own record, openvrpaths.vrpath ("runtime": [ "<SteamVR's folder>", ... ]), in
// %LOCALAPPDATA%\openvr (Windows) or ~/.config/openvr (Linux); then a copy beside this program, if there is one.
#include <openvr.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <dlfcn.h>
#endif

namespace {

namespace fs = std::filesystem;

// The runtime folders openvrpaths.vrpath lists, in order.
std::vector<fs::path> runtimeFolders() {
    fs::path file;
#ifdef _WIN32
    if (const wchar_t* l = _wgetenv(L"LOCALAPPDATA"); l && *l) file = fs::path(l) / L"openvr" / L"openvrpaths.vrpath";   // (as set: a test can point it elsewhere)
    else {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) file = fs::path(p) / L"openvr" / L"openvrpaths.vrpath";
        CoTaskMemFree(p);
    }
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    file = xdg && *xdg ? fs::path(xdg) / "openvr" / "openvrpaths.vrpath" : fs::path(home ? home : "") / ".config" / "openvr" / "openvrpaths.vrpath";
#endif
    std::ifstream in(file, std::ios::binary);
    const std::string j((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<fs::path> out;
    size_t at = j.find("\"runtime\"");
    if (at == std::string::npos) return out;
    at = j.find('[', at);
    const size_t end = at == std::string::npos ? at : j.find(']', at);
    while (at != std::string::npos && at < end) {     // each "..." in the array, JSON escapes undone
        const size_t q = j.find('"', at + 1);
        if (q == std::string::npos || q > end) break;
        std::string s;
        size_t k = q + 1;
        for (; k < j.size() && j[k] != '"'; ++k) {
            if (j[k] == '\\' && k + 1 < j.size()) ++k;
            s += j[k];
        }
        if (!s.empty()) out.push_back(fs::u8path(s));
        at = k + 1;
    }
    return out;
}

#ifdef _WIN32
using Lib = HMODULE;
Lib open(const fs::path& p) { return LoadLibraryW(p.wstring().c_str()); }
void* symbol(Lib l, const char* name) { return reinterpret_cast<void*>(GetProcAddress(l, name)); }
const char* kLibInRuntime = "bin/win64/openvr_api.dll";
const char* kLibName = "openvr_api.dll";
#else
using Lib = void*;
Lib open(const fs::path& p) { return dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL); }
void* symbol(Lib l, const char* name) { return dlsym(l, name); }
#if defined(__aarch64__)
const char* kLibInRuntime = "bin/linuxarm64/libopenvr_api.so";
#else
const char* kLibInRuntime = "bin/linux64/libopenvr_api.so";
#endif
const char* kLibName = "libopenvr_api.so";
#endif

Lib library() {
    static const Lib lib = [] {
        for (const fs::path& r : runtimeFolders())
            if (const Lib l = open(r / fs::u8path(kLibInRuntime))) return l;
        return open(fs::u8path(kLibName));            // (one beside the program, or on the search path)
    }();
    return lib;
}

template <typename F> F entry(const char* name) {
    const Lib l = library();
    return l ? reinterpret_cast<F>(symbol(l, name)) : nullptr;
}

}  // namespace

namespace vr {

uint32_t VR_CALLTYPE VR_InitInternal2(EVRInitError* peError, EVRApplicationType eApplicationType, const char* pStartupInfo) {
    using F = uint32_t(VR_CALLTYPE*)(EVRInitError*, EVRApplicationType, const char*);
    static const F f = entry<F>("VR_InitInternal2");
    if (!f) { if (peError) *peError = VRInitError_Init_InstallationNotFound; return 0; }
    return f(peError, eApplicationType, pStartupInfo);
}

void VR_CALLTYPE VR_ShutdownInternal() {
    using F = void(VR_CALLTYPE*)();
    static const F f = entry<F>("VR_ShutdownInternal");
    if (f) f();
}

void* VR_CALLTYPE VR_GetGenericInterface(const char* pchInterfaceVersion, EVRInitError* peError) {
    using F = void*(VR_CALLTYPE*)(const char*, EVRInitError*);
    static const F f = entry<F>("VR_GetGenericInterface");
    if (!f) { if (peError) *peError = VRInitError_Init_InstallationNotFound; return nullptr; }
    return f(pchInterfaceVersion, peError);
}

bool VR_CALLTYPE VR_IsInterfaceVersionValid(const char* pchInterfaceVersion) {
    using F = bool(VR_CALLTYPE*)(const char*);
    static const F f = entry<F>("VR_IsInterfaceVersionValid");
    return f && f(pchInterfaceVersion);
}

uint32_t VR_CALLTYPE VR_GetInitToken() {
    using F = uint32_t(VR_CALLTYPE*)();
    static const F f = entry<F>("VR_GetInitToken");
    return f ? f() : 0;
}

bool VR_CALLTYPE VR_IsHmdPresent() {
    using F = bool(VR_CALLTYPE*)();
    static const F f = entry<F>("VR_IsHmdPresent");
    return f && f();
}

bool VR_CALLTYPE VR_IsRuntimeInstalled() {
    using F = bool(VR_CALLTYPE*)();
    static const F f = entry<F>("VR_IsRuntimeInstalled");
    return f && f();
}

bool VR_GetRuntimePath(char* pchPathBuffer, uint32_t unBufferSize, uint32_t* punRequiredBufferSize) {
    using F = bool (*)(char*, uint32_t, uint32_t*);
    static const F f = entry<F>("VR_GetRuntimePath");
    return f && f(pchPathBuffer, unBufferSize, punRequiredBufferSize);
}

const char* VR_CALLTYPE VR_GetVRInitErrorAsSymbol(EVRInitError error) {
    using F = const char*(VR_CALLTYPE*)(EVRInitError);
    static const F f = entry<F>("VR_GetVRInitErrorAsSymbol");
    return f ? f(error) : "VRInitError_Init_InstallationNotFound";
}

const char* VR_CALLTYPE VR_GetVRInitErrorAsEnglishDescription(EVRInitError error) {
    using F = const char*(VR_CALLTYPE*)(EVRInitError);
    static const F f = entry<F>("VR_GetVRInitErrorAsEnglishDescription");
    return f ? f(error) : "SteamVR isn't installed (its OpenVR library wasn't found)";
}

}  // namespace vr
