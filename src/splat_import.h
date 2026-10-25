#pragma once
// Gaussian splats (3D Gaussian Splatting captures: Polycam, Luma, Scaniverse, SuperSplat, Postshot, Niantic) read
// into one list, whatever the file: the 3DGS training .ply, antimatter15's .splat and Niantic's .spz. Each splat is a
// soft 3D blob: a centre, its size along three axes, a rotation, a colour and how opaque it is. Only the base colour
// is kept (the view-dependent spherical harmonics beyond degree 0 are dropped: most of the bytes, little to see on
// the headset). Axes become Frame's: metres, +Y up, right-handed (glTF's), whatever the file used.
#include <cstdint>
#include <string>
#include <vector>

struct Splat {
    float pos[3];           // centre (metres)
    float scale[3];         // the blob's radius (one standard deviation) along its own x, y, z (metres)
    float rot[4];           // its rotation, a unit quaternion x y z w
    uint8_t rgba[4];        // base colour (sRGB) and opacity
};

struct SplatCloud {
    std::vector<Splat> splats;
    std::string format;     // "3DGS .ply", ".splat", ".spz"
};

// .splat, .spz, and a .ply whose vertices carry splat properties (f_dc_0, scale_0, ...): the file is read to tell.
bool isSplatPath(const std::string& path);
bool loadSplats(const std::string& path, SplatCloud& out, std::string& error);
// The same from bytes already in memory (a .fenv member); `ext` says which format (".spz", ".splat", ".ply").
bool loadSplatBytes(const std::vector<uint8_t>& bytes, const std::string& ext, SplatCloud& out, std::string& error);

// How a .fenv carries splats: .spz (version 2, Frame's axes), 19 bytes a splat before its gzip (about 12 after). Positions to 1/4096 m within
// 2 km of the object's origin; sizes, colours and rotations to 8 bits.
std::vector<uint8_t> writeSpz(const std::vector<Splat>& splats);

// A splat's 3D covariance (its shape: R S S^T R^T) as the six distinct entries xx xy xz yy yz zz.
void splatCovariance(const Splat& s, float cov[6]);
