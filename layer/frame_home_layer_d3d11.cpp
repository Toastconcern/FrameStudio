// Frame Home's Direct3D 11 compositor layer (Windows): the same idea as the Vulkan layer on the Frame and Linux
// (layer/frame_home_layer.cpp), but SteamVR's Windows compositor is Direct3D 11, so this lives inside it as a proxy
// d3d11.dll. D3D11CreateDevice is ours (d3d11_proxy.def forwards every other export to the real d3d11.dll, copied
// beside us as fh_d3d11real.dll), so when vrcompositor creates its device we wrap it and swap only the environment:
// where the compositor draws Aurora's sky we draw the newest frame Frame Home (frame-home --system) rendered of your
// home, looked up per pixel by direction (rotation reprojection), and Aurora's floor and stars we leave undrawn.
// Everything else the compositor does is untouched, so the home shows at boot, during game loads, behind the Steam
// menu, with the controllers and passthrough, exactly as Aurora would. Outside vrcompositor this is pure pass-through.
//
// Hand-over (env_share.h): Frame Home connects to our named pipe and sends its shared State's handle and the size it
// wants; we make kSlots shared textures, hand it their NT handles, and it renders the home into them over Vulkan.
//
// Safety:
//   active only inside a process named vrcompositor.exe; everywhere else D3D11CreateDevice just calls the real one
//   no published frame, or one older than half a second (Frame Home stopped or crashed): Aurora as usual
//   %LOCALAPPDATA%\Frame Home\layer\off: pass-through (checked at start and each second)
//   twice the compositor started with us swapping and never got a healthy frame in: we write `off` ourselves
//   deleting fh_d3d11real.dll / this d3d11.dll from SteamVR's bin\win64 removes us entirely (the installer does)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../src/env_share.h"

namespace fs = std::filesystem;

namespace {

// ── process / files ──────────────────────────────────────────────────────────────────────────────

bool isCompositor() {
    static int v = -1;
    if (v < 0) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        v = _wcsicmp(base, L"vrcompositor.exe") == 0;
    }
    return v == 1;
}

// %LOCALAPPDATA%\Frame Home (home_platform.h's root() on Windows), where the logs live.
fs::path dir() {
    wchar_t* p = nullptr;
    size_t n = 0;
    _wdupenv_s(&p, &n, L"LOCALAPPDATA");
    fs::path d = p ? fs::path(p) / L"Frame Home" : fs::path(L"Frame Home");
    free(p);
    return d;
}
// The layer's own folder (root\layer): its off switch and crash guard, the same names and place the Environments
// panel uses (rootDir()/layer/off, .../crashes, .../starting) and the installer makes to mark the layer present.
fs::path layerDir() { return dir() / L"layer"; }
bool fileExists(const fs::path& p) { std::error_code ec; return fs::exists(p, ec); }

std::mutex g_logMutex;
void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> l(g_logMutex);
    std::error_code ec;
    fs::create_directories(dir() / "logs", ec);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir() / "logs" / "frame-layer.log").wstring().c_str(), L"a") || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

std::vector<char> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

uint64_t fnv(const void* data, size_t n) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char* b = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// ── Aurora shaders, by the bytecode the compositor hands CreatePixelShader ─────────────────────────
// The compositor loads SteamVR's compiled pixel shaders (resources\shaders\d3d11\*.fxo) and creates them with the
// file's bytes. We hash each file whose name marks it part of the environment and, at CreatePixelShader, tag the
// ID3D11PixelShader it returns: the sky (and horizon) we replace with the home; the floor and stars we skip.
enum class Kind { None, Replace, Skip };
std::unordered_map<uint64_t, Kind> g_shaderKinds;     // bytecode hash -> what to do

std::unordered_map<uint64_t, std::string> g_shaderName;   // bytecode hash -> .fxo name, every pixel shader (diagnostic)

void scanAuroraShaders() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);       // ...\SteamVR\bin\win64\vrcompositor.exe
    fs::path shaders = fs::path(path).parent_path().parent_path().parent_path() / L"resources" / L"shaders" / L"d3d11";
    std::error_code ec;
    int replace = 0, skip = 0;
    for (auto& e : fs::directory_iterator(shaders, ec)) {
        if (e.path().extension() != ".fxo") continue;
        const std::string name = e.path().filename().string();
        if (name.find("_ps.fxo") == std::string::npos) continue;
        const std::vector<char> b = readFile(e.path());
        if (b.empty()) continue;
        const uint64_t h = fnv(b.data(), b.size());
        g_shaderName[h] = name;                        // every pixel shader, so a draw can be named (diagnostic)
        if (name.find("aurora") == std::string::npos && name.find("motes") == std::string::npos) continue;
        // Aurora's sky and horizon (what you look out at) we draw the home into; its floor and its stars (the motes
        // "sky" that is really the floor's ground glow) we leave off, as the Vulkan layer does.
        Kind k = name.find("floor") != std::string::npos || name.find("motes") != std::string::npos ? Kind::Skip : Kind::Replace;
        g_shaderKinds[h] = k;
        (k == Kind::Replace ? replace : skip) += 1;
    }
    logf("aurora shaders: %d replaced, %d skipped, %zu pixel shaders named (from %s)", replace, skip, g_shaderName.size(), shaders.string().c_str());
}

// ── the home: shared state, the compositor's textures, and our draw resources ──────────────────────

// Two locks, never held in the reverse order: g_shaderMutex guards the recognised shaders (written as the compositor
// creates them, read at each draw); g_texMutex guards the shared textures (rebuilt by the pipe thread when Frame Home
// connects or resizes, read while a draw is replaced). A replaced draw holds g_texMutex across the draw so the pipe
// thread can't free a texture mid-draw — the race that crashed the compositor.
std::mutex g_shaderMutex, g_texMutex;
std::atomic<bool> g_off{false};
std::atomic<bool> g_connected{false};
std::atomic<uint64_t> g_replaced{0}, g_skipped{0};     // draws swapped for the home / left undrawn (a diagnostic)

envshare::State* g_state = nullptr;                    // Frame Home's shared State (it owns it; we map its handle)
HANDLE g_stateMap = nullptr;

ID3D11Device* g_device = nullptr;                      // the compositor's device (not addref'd: it outlives us)
ID3D11DeviceContext* g_context = nullptr;              // its immediate context

uint32_t g_generation = 0;                             // bumped each time Frame Home asks for new textures
uint32_t g_texW = 0, g_texH = 0;
struct SharedTex { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; HANDLE handle = nullptr; };
std::vector<SharedTex> g_tex;                          // kSlots, on the compositor's device
int g_lastGood = -1;                                   // newest slot drawn (held while Frame Home makes new textures)

// Our draw resources on the compositor's device.
ID3D11PixelShader* g_homePs = nullptr;
ID3D11Buffer* g_homeCb = nullptr;                      // HomeCb below (b13): reprojection + brightness
ID3D11SamplerState* g_sampler = nullptr;
bool g_resourcesTried = false, g_resourcesOk = false;

constexpr UINT kSrvSlot = 13, kCbSlot = 13, kSampSlot = 13;   // high slots the compositor's own shaders don't use

struct HomeCb {                                        // matches the cbuffer in the replacement shader (b13)
    float col[envshare::kSlots == 0 ? 1 : 2][4][4];    // per eye: the 4 columns of projection*viewRotation
    float eye[2][4];                                   // per eye position (xyz)
    float params[4];                                   // x: brightness (fade), y: 1 when there's something to show
};

// Per command buffer? D3D's immediate context is one object; the pixel shader bound on it, tracked by PSSetShader.
ID3D11PixelShader* g_boundPs = nullptr;
std::unordered_map<ID3D11PixelShader*, std::string> g_psName;          // every recognised shader's .fxo name (diagnostic)
std::unordered_map<ID3D11DeviceContext*, ID3D11PixelShader*> g_boundByCtx;   // the pixel shader bound on each context

// Which slot to draw this compositor frame: picked at the first replaced draw and held briefly, so both eyes and
// every aurora piece in the frame show the same home frame (frameSlot in the Vulkan layer).
int g_latchSlot = -1;
uint64_t g_latchNs = 0;
bool g_latchStamped = false;
constexpr uint64_t kLatchNs = 4000000ull;

int liveSlot() {   // the newest published slot, or -1 for Aurora (Frame Home gone or frozen 5 s)
    if (g_off || !g_state || g_tex.empty()) return -1;
    const uint64_t now = envshare::nowNs();
    if (now - g_state->publishNs.load() > 5000000000ull && now > g_state->busyUntilNs.load()) return -1;
    const uint32_t k = g_state->latest.load(std::memory_order_acquire);
    if (k < envshare::kSlots && g_state->slot[k].generation == g_generation) g_lastGood = static_cast<int>(k);
    return g_lastGood;
}
int frameSlot() {
    const uint64_t now = envshare::nowNs();
    if (g_latchNs && now - g_latchNs < kLatchNs) return g_latchSlot;
    g_latchSlot = liveSlot();
    g_latchNs = now;
    g_latchStamped = false;
    return g_latchSlot;
}

// ── health: off switch and crash guard (mirrors the Vulkan layer) ──────────────────────────────────
std::atomic<bool> g_armed{false};
uint64_t g_healthyFrames = 0;

void startHealth() {
    std::error_code ec;
    fs::create_directories(layerDir(), ec);
    if (fileExists(layerDir() / "off")) { g_off = true; logf("off: Aurora as usual (delete %s\\off to turn Frame Home back on)", dir().string().c_str()); return; }
    int crashes = 0;
    if (fileExists(layerDir() / "starting")) {
        const std::vector<char> c = readFile(layerDir() / "crashes");
        crashes = c.empty() ? 1 : std::atoi(std::string(c.begin(), c.end()).c_str()) + 1;
        if (FILE* f = nullptr; !_wfopen_s(&f, (layerDir() / "crashes").wstring().c_str(), L"w") && f) { std::fprintf(f, "%d\n", crashes); std::fclose(f); }
    }
    if (crashes >= 2) {
        if (FILE* f = nullptr; !_wfopen_s(&f, (layerDir() / "off").wstring().c_str(), L"w") && f) std::fclose(f);
        DeleteFileW((layerDir() / "starting").wstring().c_str());
        g_off = true;
        logf("the compositor stopped twice before a healthy frame: turned myself off (Aurora as usual)");
    }
}
void arm() {   // we are about to start swapping Aurora: a crash from here that never got healthy counts
    if (g_armed.exchange(true)) return;
    if (FILE* f = nullptr; !_wfopen_s(&f, (layerDir() / "starting").wstring().c_str(), L"w") && f) std::fclose(f);
}
void healthy() {   // a frame drawn and shown: the compositor got this far, so a later crash isn't ours
    if (++g_healthyFrames == 30 && g_armed.exchange(false)) {
        DeleteFileW((layerDir() / "starting").wstring().c_str());
        DeleteFileW((layerDir() / "crashes").wstring().c_str());
    }
}

// ── the replacement pixel shader, compiled at startup ──────────────────────────────────────────────
// Reads the compositor's own constant buffer (b0) for this eye's position, forms the direction from the eye to the
// drawn point, looks it up in the home frame (projection * view rotation, column major, as Frame Home rendered it)
// and samples that eye's layer. The same maths as layer/env_ps.frag.
const char* kHlsl = R"(
Texture2DArray<float4> home : register(t13);
SamplerState samp : register(s13);
cbuffer Aurora : register(b0) { float4 _pad0[28]; float4 g_vEyePositionWs; }   // g_vEyePositionWs at offset 448
cbuffer Home : register(b13) {
    float4 uCol[8];     // per eye (0: 0..3, 1: 4..7): the 4 columns of projection*viewRotation, tracking space
    float4 uEyePos[2];  // per eye position Frame Home rendered from, tracking space (xyz)
    float4 uParams;     // x: brightness, y: 1 when there's something to show
};
struct PSIn { float4 pos : SV_Position; float3 worldPos : TEXCOORD1; };
float4 main(PSIn i) : SV_Target {
    float3 eye = g_vEyePositionWs.xyz;
    float3 dir = normalize(i.worldPos - eye);
    int k = distance(eye, uEyePos[0].xyz) <= distance(eye, uEyePos[1].xyz) ? 0 : 1;
    int b = k * 4;
    float4 c = uCol[b + 0] * dir.x + uCol[b + 1] * dir.y + uCol[b + 2] * dir.z;
    float3 rgb = float3(0, 0, 0);
    if (c.w > 1e-5 && uParams.y > 0.5) {
        float2 uv = (c.xy / c.w) * 0.5 + 0.5;
        rgb = home.SampleLevel(samp, float3(uv, (float)k), 0.0).rgb;
    }
    return float4(rgb * uParams.x, 1.0);
}
)";

ID3DBlob* compileHlsl() {
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!comp) { logf("no d3dcompiler_47.dll: Aurora stays"); return nullptr; }
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto d3dCompile = reinterpret_cast<Fn>(GetProcAddress(comp, "D3DCompile"));
    if (!d3dCompile) return nullptr;
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = d3dCompile(kHlsl, std::strlen(kHlsl), "frame_home_env", nullptr, nullptr, "main", "ps_5_0", 0, 0, &code, &err);
    if (FAILED(hr)) { logf("replacement shader failed to compile (0x%08lx): %s", hr, err ? static_cast<const char*>(err->GetBufferPointer()) : ""); if (err) err->Release(); return nullptr; }
    if (err) err->Release();
    return code;
}

bool ensureResources() {
    if (g_resourcesTried) return g_resourcesOk;
    g_resourcesTried = true;
    ID3DBlob* code = compileHlsl();
    if (!code) return false;
    if (FAILED(g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_homePs))) { code->Release(); logf("CreatePixelShader for the home failed"); return false; }
    code->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(HomeCb);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device->CreateBuffer(&bd, nullptr, &g_homeCb))) { logf("home cbuffer failed"); return false; }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_device->CreateSamplerState(&sd, &g_sampler))) { logf("home sampler failed"); return false; }
    g_resourcesOk = true;
    logf("home draw resources ready");
    return true;
}

void fillCb(int slot) {
    if (!g_homeCb) return;
    const envshare::Slot& s = g_state->slot[slot];
    HomeCb cb = {};
    for (int e = 0; e < 2; ++e) {
        std::memcpy(cb.col[e], s.viewProj[e], sizeof(float) * 16);   // column major: 4 columns of 4 floats
        for (int c = 0; c < 4; ++c) cb.eye[e][c] = s.eyePos[e][c];
    }
    cb.params[0] = g_state->fade.load() / 1000.0f;
    cb.params[1] = g_state->show.load() ? 1.0f : 0.0f;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_context->Map(g_homeCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &cb, sizeof cb);
        g_context->Unmap(g_homeCb, 0);
    }
}

// ── the hooks: CreatePixelShader on the device, PSSetShader + the four draws on the context ─────────

using PFN_CreatePixelShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using PFN_PSSetShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);

PFN_CreatePixelShader g_realCreatePixelShader = nullptr;
PFN_PSSetShader g_realPSSetShader = nullptr;
PFN_Draw g_realDraw = nullptr;
PFN_DrawIndexed g_realDrawIndexed = nullptr;
PFN_DrawInstanced g_realDrawInstanced = nullptr;
PFN_DrawIndexedInstanced g_realDrawIndexedInstanced = nullptr;

std::unordered_map<ID3D11PixelShader*, Kind> g_psKind;   // shaders the compositor made that we recognised

HRESULT STDMETHODCALLTYPE CreatePixelShader_hook(ID3D11Device* dev, const void* code, SIZE_T len, ID3D11ClassLinkage* link, ID3D11PixelShader** out) {
    const HRESULT hr = g_realCreatePixelShader(dev, code, len, link, out);
    if (SUCCEEDED(hr) && out && *out && code && len) {
        const uint64_t h = fnv(code, len);
        std::lock_guard<std::mutex> l(g_shaderMutex);
        if (auto it = g_shaderKinds.find(h); it != g_shaderKinds.end()) g_psKind[*out] = it->second;
        if (auto it = g_shaderName.find(h); it != g_shaderName.end()) g_psName[*out] = it->second;
    }
    return hr;
}

void STDMETHODCALLTYPE PSSetShader_hook(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* inst, UINT n) {
    if (ctx == g_context) g_boundPs = ps;
    { std::lock_guard<std::mutex> l(g_shaderMutex); g_boundByCtx[ctx] = ps; }   // per context (the environment may use a deferred one)
    g_realPSSetShader(ctx, ps, inst, n);
}

// Diagnostic: the first time each known pixel shader is drawn, note it and the context, so one headset session shows
// exactly what the compositor draws the environment with (and whether it's our immediate context or another).
std::unordered_set<std::string> g_seenHere, g_seenOther;
std::atomic<int> g_seenCount{0};
void noteDraw(ID3D11DeviceContext* ctx) {
    static std::atomic<uint64_t> calls{0};
    if (g_off || calls.fetch_add(1, std::memory_order_relaxed) > 500000 || g_seenCount.load(std::memory_order_relaxed) >= 200) return;   // a few seconds of draws, then nothing
    std::string name;
    {
        std::lock_guard<std::mutex> l(g_shaderMutex);
        auto b = g_boundByCtx.find(ctx);
        if (b == g_boundByCtx.end() || !b->second) return;
        auto it = g_psName.find(b->second);
        if (it == g_psName.end()) return;
        auto& seen = ctx == g_context ? g_seenHere : g_seenOther;
        if (!seen.insert(it->second).second) return;
        name = it->second;
        ++g_seenCount;
    }
    logf("compositor draws %s%s", name.c_str(), ctx == g_context ? "" : " (on another context, not the one we track)");
}

// What the compositor's about-to-draw pixel shader means for us (no texture access: that's under g_texMutex).
Kind classify(ID3D11DeviceContext* ctx) {
    if (g_off || ctx != g_context || !g_boundPs) return Kind::None;
    std::lock_guard<std::mutex> l(g_shaderMutex);
    auto it = g_psKind.find(g_boundPs);
    return it == g_psKind.end() ? Kind::None : it->second;
}

// Binds the home over the compositor's state for one draw, then puts the shader back.
struct HomeBind {
    ID3D11DeviceContext* ctx;
    ID3D11PixelShader* restore;
    ID3D11ShaderResourceView* srv;
    HomeBind(ID3D11DeviceContext* c, int slot, ID3D11PixelShader* r) : ctx(c), restore(r), srv(g_tex[slot].srv) {
        g_realPSSetShader(ctx, g_homePs, nullptr, 0);
        ctx->PSSetShaderResources(kSrvSlot, 1, &srv);
        ctx->PSSetConstantBuffers(kCbSlot, 1, &g_homeCb);
        ctx->PSSetSamplers(kSampSlot, 1, &g_sampler);
    }
    ~HomeBind() {
        g_realPSSetShader(ctx, restore, nullptr, 0);   // the compositor's aurora shader back, so our tracking holds
        healthy();
    }
};

// Runs the compositor's draw, swapping the home in for an Aurora sky draw or dropping an Aurora floor/stars draw.
// The texture lock is held across a replaced draw so the pipe thread can't free the texture under it.
template <typename Draw>
void replaceDraw(ID3D11DeviceContext* ctx, Draw&& draw) {
    noteDraw(ctx);                                   // diagnostic: what the compositor draws, and on which context
    const Kind k = classify(ctx);
    if (k == Kind::None) { draw(); return; }
    if (k == Kind::Skip) { g_skipped.fetch_add(1, std::memory_order_relaxed); return; }
    std::unique_lock<std::mutex> lock(g_texMutex);
    const int slot = frameSlot();
    if (slot < 0 || !ensureResources() || slot >= static_cast<int>(g_tex.size()) || !g_tex[slot].srv) { lock.unlock(); draw(); return; }
    fillCb(slot);
    if (!g_latchStamped) { g_latchStamped = true; g_state->sampledNs.store(g_latchNs); }
    g_replaced.fetch_add(1, std::memory_order_relaxed);
    HomeBind b(ctx, slot, g_boundPs);
    draw();
}

void STDMETHODCALLTYPE Draw_hook(ID3D11DeviceContext* ctx, UINT n, UINT start) {
    replaceDraw(ctx, [&] { g_realDraw(ctx, n, start); });
}
void STDMETHODCALLTYPE DrawIndexed_hook(ID3D11DeviceContext* ctx, UINT n, UINT start, INT base) {
    replaceDraw(ctx, [&] { g_realDrawIndexed(ctx, n, start, base); });
}
void STDMETHODCALLTYPE DrawInstanced_hook(ID3D11DeviceContext* ctx, UINT vpi, UINT inst, UINT sv, UINT si) {
    replaceDraw(ctx, [&] { g_realDrawInstanced(ctx, vpi, inst, sv, si); });
}
void STDMETHODCALLTYPE DrawIndexedInstanced_hook(ID3D11DeviceContext* ctx, UINT ipi, UINT inst, UINT si, INT bv, UINT sinst) {
    replaceDraw(ctx, [&] { g_realDrawIndexedInstanced(ctx, ipi, inst, si, bv, sinst); });
}

// Swaps one vtable entry for our hook, keeping the original. (Many instances share one class vtable, so hooking the
// compositor's device and immediate context hooks them all; we gate on g_device/g_context anyway.)
template <typename T>
void hookSlot(void* obj, int index, void* hook, T* real) {
    void** vtable = *reinterpret_cast<void***>(obj);
    DWORD old = 0;
    if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        *real = reinterpret_cast<T>(vtable[index]);
        vtable[index] = hook;
        VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    }
}

// ── the hand-over pipe: Frame Home asks for textures, we make them and share them ──────────────────

void destroyTextures() {
    for (SharedTex& t : g_tex) {
        if (t.srv) t.srv->Release();
        if (t.tex) t.tex->Release();
        if (t.handle) CloseHandle(t.handle);
    }
    g_tex.clear();
    g_lastGood = -1;
}

bool makeTextures(uint32_t w, uint32_t h) {
    destroyTextures();
    g_tex.resize(envshare::kSlots);
    for (uint32_t i = 0; i < envshare::kSlots; ++i) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 2;                              // one layer per eye
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_tex[i].tex))) { logf("shared texture %u failed", i); destroyTextures(); return false; }
        D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
        vd.Format = td.Format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.ArraySize = 2;
        if (FAILED(g_device->CreateShaderResourceView(g_tex[i].tex, &vd, &g_tex[i].srv))) { logf("shared srv %u failed", i); destroyTextures(); return false; }
        IDXGIResource1* res = nullptr;
        if (FAILED(g_tex[i].tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&res))) ||
            FAILED(res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &g_tex[i].handle))) {
            if (res) res->Release();
            logf("shared handle %u failed", i);
            destroyTextures();
            return false;
        }
        res->Release();
    }
    g_texW = w;
    g_texH = h;
    return true;
}

void adapterLuid(uint8_t out[8]) {
    std::memset(out, 0, 8);
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC d;
        if (SUCCEEDED(adapter->GetDesc(&d))) std::memcpy(out, &d.AdapterLuid, 8);
    }
    if (adapter) adapter->Release();
    if (dxgi) dxgi->Release();
}

void handleRequest(HANDLE pipe, DWORD clientDefaultPid) {
    envshare::WinRequest req = {};
    DWORD got = 0;
    if (!ReadFile(pipe, &req, sizeof req, &got, nullptr) || got != sizeof req || req.magic != envshare::kMagic) return;
    envshare::WinReply reply = {};
    reply.magic = envshare::kMagic;
    reply.generation = req.generation;

    std::lock_guard<std::mutex> l(g_texMutex);   // the draw path reads g_state and g_tex under this
    // Frame Home's shared State: the handle it duplicated into us. Map it (a new mapping replaces an old one).
    if (req.state) {
        if (g_state) { UnmapViewOfFile(g_state); g_state = nullptr; }
        if (g_stateMap) { CloseHandle(g_stateMap); g_stateMap = nullptr; }
        g_stateMap = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(req.state));
        g_state = static_cast<envshare::State*>(MapViewOfFile(g_stateMap, FILE_MAP_ALL_ACCESS, 0, 0, req.stateSize ? req.stateSize : sizeof(envshare::State)));
        if (!g_state) { logf("could not map Frame Home's state"); }
    }
    if (g_device && g_state && makeTextures(req.width, req.height)) {
        g_generation = req.generation;
        g_lastGood = -1;
        DWORD pid = req.pid ? req.pid : clientDefaultPid;
        HANDLE client = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
        bool ok = client != nullptr;
        for (uint32_t i = 0; i < envshare::kSlots && ok; ++i) {
            HANDLE dup = nullptr;
            ok = DuplicateHandle(GetCurrentProcess(), g_tex[i].handle, client, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
            reply.texture[i] = reinterpret_cast<uint64_t>(dup);
        }
        if (client) CloseHandle(client);
        if (ok) {
            adapterLuid(reply.deviceLUID);
            reply.ok = 1;
            arm();
            logf("Frame Home connected: %ux%u, generation %u", req.width, req.height, req.generation);
        } else {
            logf("could not share the textures with Frame Home (pid %lu)", pid);
        }
    }
    DWORD wrote = 0;
    WriteFile(pipe, &reply, sizeof reply, &wrote, nullptr);
    FlushFileBuffers(pipe);
}

void serve() {
    g_connected = false;
    for (;;) {
        if (fileExists(layerDir() / "off")) { g_off = true; return; }
        HANDLE pipe = CreateNamedPipeA(envshare::kPipeName, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
                                       sizeof(envshare::WinReply) + 64, sizeof(envshare::WinRequest) + 64, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            ULONG pid = 0;
            GetNamedPipeClientProcessId(pipe, &pid);
            g_connected = true;
            handleRequest(pipe, pid);
            // Hold the connection: when it breaks, Frame Home has gone and we drop to Aurora.
            char b;
            DWORD n;
            while (ReadFile(pipe, &b, 0, &n, nullptr) || GetLastError() == ERROR_MORE_DATA) {
                if (fileExists(layerDir() / "off")) break;
            }
            g_connected = false;
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

// Watches the off switch each second (turn Frame Home off/on without restarting SteamVR).
void watchOff() {
    uint64_t sec = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const bool off = fileExists(layerDir() / "off");
        if (off != g_off.load()) { g_off = off; logf(off ? "turned off: Aurora as usual" : "turned back on"); }
        // A heartbeat (every 5 s) of what the layer is doing, for testing without a headset to look through.
        if (++sec % 5 == 0) {
            const uint64_t r = g_replaced.exchange(0), s = g_skipped.exchange(0);
            size_t here, other;
            { std::lock_guard<std::mutex> l(g_shaderMutex); here = g_seenHere.size(); other = g_seenOther.size(); }
            logf("heartbeat: home over %llu aurora draws, %llu undrawn (5 s); distinct shaders drawn here=%zu other=%zu; connected=%d",
                 (unsigned long long)r, (unsigned long long)s, here, other, g_connected.load() ? 1 : 0);
        }
    }
}

void wrap(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    static std::once_flag once;
    std::call_once(once, [&] {
        g_device = dev;
        g_context = ctx;
        g_device->AddRef();   // our own long-lived references (the context's is the one GetImmediateContext gave us),
                              // so the vtables we hook stay valid however the compositor manages its own copies
        startHealth();
        if (g_off) { logf("vrcompositor device wrapped, but Frame Home is off: Aurora as usual"); return; }
        scanAuroraShaders();
        hookSlot(dev, 15, reinterpret_cast<void*>(CreatePixelShader_hook), &g_realCreatePixelShader);
        hookSlot(ctx, 9, reinterpret_cast<void*>(PSSetShader_hook), &g_realPSSetShader);
        hookSlot(ctx, 12, reinterpret_cast<void*>(DrawIndexed_hook), &g_realDrawIndexed);
        hookSlot(ctx, 13, reinterpret_cast<void*>(Draw_hook), &g_realDraw);
        hookSlot(ctx, 20, reinterpret_cast<void*>(DrawIndexedInstanced_hook), &g_realDrawIndexedInstanced);
        hookSlot(ctx, 21, reinterpret_cast<void*>(DrawInstanced_hook), &g_realDrawInstanced);
        std::thread(serve).detach();
        std::thread(watchOff).detach();
        logf("vrcompositor device wrapped: Frame Home is the environment");
    });
}

// ── the real d3d11.dll and our D3D11CreateDevice ───────────────────────────────────────────────────

using PFN_D3D11CreateDevice = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                               ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
PFN_D3D11CreateDevice realCreateDevice() {
    static PFN_D3D11CreateDevice f = [] {
        HMODULE m = LoadLibraryW(L"fh_d3d11real.dll");   // the real d3d11.dll, copied beside us by the installer
        return m ? reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(m, "D3D11CreateDevice")) : nullptr;
    }();
    return f;
}

}  // namespace

extern "C" HRESULT WINAPI D3D11CreateDevice(   // exported by layer/d3d11_proxy.def
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdk,
    ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    PFN_D3D11CreateDevice real = realCreateDevice();
    if (!real) return E_FAIL;
    // Exactly as the caller asked (no tampering with its device/context or their lifetimes). If it's the compositor
    // and it got a device, we take our own reference to the device and its immediate context (wrap() AddRefs them)
    // and keep them for the process — so hooking their vtables is safe however the compositor manages its copies.
    const HRESULT hr = real(adapter, driverType, software, flags, levels, numLevels, sdk, device, level, context);
    if (SUCCEEDED(hr) && isCompositor() && device && *device) {
        ID3D11DeviceContext* immediate = nullptr;
        (*device)->GetImmediateContext(&immediate);      // +1 ref, kept by wrap()
        if (immediate) wrap(*device, immediate);
    }
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH && isCompositor()) realCreateDevice();   // load the real one early
    return TRUE;
}
