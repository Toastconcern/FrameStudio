#pragma once

#include <array>

namespace gltfmaterial {

struct TextureTintInput {
    bool usesBaseColorTexture = false;
    bool usesEmissiveTexture = false;
    bool hasBaseColorFactor = false;
    bool hasEmissiveFactor = false;
    std::array<float, 4> baseColorFactor{1.f, 1.f, 1.f, 1.f};
    std::array<float, 3> emissiveFactor{1.f, 1.f, 1.f};
};

// The legacy V79 loader converts either a base-colour texture or, when that is
// absent, an emissive texture into the cooker's single unlit texture slot. The
// selected texture must be multiplied by the factor that belongs to that same
// glTF channel. In particular, an emissive-only material commonly has a black
// baseColorFactor; applying it to the emissive texture makes the entire mesh
// disappear in both the editor preview and the cooked Home.
inline std::array<float, 4> selectedTextureTint(const TextureTintInput& in) {
    std::array<float, 4> out{1.f, 1.f, 1.f, 1.f};
    if (in.usesBaseColorTexture) {
        if (in.hasBaseColorFactor) out = in.baseColorFactor;
        return out;
    }
    if (in.usesEmissiveTexture) {
        if (in.hasEmissiveFactor) {
            out[0] = in.emissiveFactor[0];
            out[1] = in.emissiveFactor[1];
            out[2] = in.emissiveFactor[2];
        }
        // Alpha remains a surface property even when RGB comes from emission.
        if (in.hasBaseColorFactor) out[3] = in.baseColorFactor[3];
        return out;
    }
    if (in.hasBaseColorFactor) out = in.baseColorFactor;
    return out;
}

}  // namespace gltfmaterial
