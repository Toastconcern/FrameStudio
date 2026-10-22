#pragma once
// SteamVR's panels (overlays) of every app. OpenVR can't list them, but a handle is (slot << 32 | serial) with both
// small, so probing that range finds them all; a whole sweep takes about 0.1 s. Used to tell whether a hand points
// at a panel (frame-switcher).
#include <openvr.h>

#include <cstring>
#include <string>
#include <vector>

struct OverlayInfo {
    vr::VROverlayHandle_t handle = 0;
    std::string key;
    vr::VROverlayTransformType type = vr::VROverlayTransform_Absolute;
};

inline std::vector<OverlayInfo> listOverlays() {
    std::vector<OverlayInfo> out;
    vr::IVROverlay* ov = vr::VROverlay();
    if (!ov) return out;
    for (uint64_t slot = 0; slot < 256; ++slot)
        for (uint64_t serial = 0; serial < 1024; ++serial) {
            const vr::VROverlayHandle_t h = slot << 32 | serial;
            char key[vr::k_unVROverlayMaxKeyLength] = {};
            vr::EVROverlayError e = vr::VROverlayError_None;
            if (!ov->GetOverlayKey(h, key, sizeof key, &e) || e != vr::VROverlayError_None || !key[0]) continue;
            OverlayInfo o;
            o.handle = h;
            o.key = key;
            ov->GetOverlayTransformType(h, &o.type);
            out.push_back(std::move(o));
        }
    return out;
}

// Panels a person looks at and points at: not cursors, laser pointers, thumbnails or Frame Home's own.
inline bool isUserPanel(const OverlayInfo& o) {
    if (o.type == vr::VROverlayTransform_Cursor || o.type == vr::VROverlayTransform_DashboardThumb) return false;
    for (const char* p : {"framehome.", "system.pointer", "system.cursor", "gamescope.", "XRService"})
        if (o.key.rfind(p, 0) == 0) return false;
    return o.key.find(".thumb") == std::string::npos && o.key.find("cursor") == std::string::npos;
}
