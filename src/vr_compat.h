#pragma once
// The OpenVR calls that return a struct, made the way SteamVR expects when Frame Home is built for Windows with
// MinGW (GCC or Clang). SteamVR is built with Microsoft's compiler, whose C++ ABI passes a member function's hidden
// return pointer after `this`; MinGW's (the Itanium ABI) passes it before. Called through the C++ interface, these
// jumped to garbage (a crash in the first frame). Here they go through the interface's vtable with Microsoft's order.
// Everywhere else (Linux, the Steam Frame, an MSVC build): the plain calls.
#include <openvr.h>

namespace vrcompat {

#if defined(_WIN32) && !defined(_MSC_VER)
// slot: the method's place in the interface's vtable, as openvr.h declares it for the version asked for (IVRSystem_026).
template <typename R, typename... A> R msvcMember(void* object, int slot, A... args) {
    R result;
    using Fn = R* (*)(void*, R*, A...);   // x64: this, the return pointer, then the arguments, as Microsoft's members take them
    (*reinterpret_cast<Fn* const*>(object))[slot](object, &result, args...);
    return result;
}
inline vr::HmdMatrix44_t projectionMatrix(vr::EVREye eye, float nearZ, float farZ) {
    return msvcMember<vr::HmdMatrix44_t>(vr::VRSystem(), 1, eye, nearZ, farZ);   // IVRSystem::GetProjectionMatrix
}
inline vr::HmdMatrix34_t eyeToHead(vr::EVREye eye) {
    return msvcMember<vr::HmdMatrix34_t>(vr::VRSystem(), 5, eye);                  // IVRSystem::GetEyeToHeadTransform
}
#else
inline vr::HmdMatrix44_t projectionMatrix(vr::EVREye eye, float nearZ, float farZ) { return vr::VRSystem()->GetProjectionMatrix(eye, nearZ, farZ); }
inline vr::HmdMatrix34_t eyeToHead(vr::EVREye eye) { return vr::VRSystem()->GetEyeToHeadTransform(eye); }
#endif

}  // namespace vrcompat
