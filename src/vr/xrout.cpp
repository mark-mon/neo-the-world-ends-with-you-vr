#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include "xrout.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "../common/log.h"
#include "perf.h"
#include "vrkit_menu.h"

namespace uxr::xrout {
namespace {

constexpr int kSlots = 3;

// ------------------------------------------------------------------------------------------- shared state
struct SlotMeta { FrameInfo fi; uint32_t gen; };

struct Shared {
    std::mutex m;
    // XR -> game
    bool ready = false;
    Pose pose[2]{};
    Fov fov[2]{};
    int64_t time = 0;
    unsigned recW = 0, recH = 0;
    LUID luid{};
    char state[96] = "starting";
    // game -> XR (texture set)
    uint32_t gen = 0;                        // bumped when the shared textures are (re)created
    HANDLE handles[kSlots]{};
    UINT eyeW = 0, eyeH = 0, hudW = 0, hudH = 0, atlasW = 0, atlasH = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    int latest = -1;                         // newest submitted slot
    uint32_t submitSeq = 0;
    // control
    uint32_t recenterReq = 0, dumpReq = 0;
    float hudDistance = 1.6f, hudWidth = 1.6f, hudOffsetY = -0.05f, menuDistance = 2.0f, menuWidth = 2.4f;
    bool hudFollowHead = false;
} S;

SlotMeta g_meta[kSlots];                     // written by the game while it holds the slot's mutex
Settings g_set;
std::atomic<bool> g_quit{false};
HANDLE g_thread = nullptr;
std::atomic<bool> g_menuAllowed{false}, g_menuOpen{false};
std::atomic<uint32_t> g_beat{0};          // incremented after every xrWaitFrame
HANDLE g_beatEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
std::mutex g_pm;
perf::Acc p_wait, p_interval, p_copy, p_end;
uint32_t p_late = 0;
double p_lastWaitDone = 0, p_periodMs = 0;
std::atomic<uint32_t> st_submitted{0}, st_noSlot{0}, st_recreated{0}, st_xrFrames{0}, st_xrNew{0}, st_xrNone{0};

void SetState(const char* fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(S.m);
    if (strcmp(S.state, buf)) { strcpy_s(S.state, buf); LOGI("xr: state -> %s", buf); }
}

// ------------------------------------------------------------------------------------------- math
XrPosef PoseMul(const XrPosef& a, const XrPosef& b) {   // b expressed in a's frame
    const XrQuaternionf& q = a.orientation; const XrQuaternionf& r = b.orientation;
    XrPosef o;
    o.orientation = {q.w * r.x + q.x * r.w + q.y * r.z - q.z * r.y, q.w * r.y - q.x * r.z + q.y * r.w + q.z * r.x,
                     q.w * r.z + q.x * r.y - q.y * r.x + q.z * r.w, q.w * r.w - q.x * r.x - q.y * r.y - q.z * r.z};
    XrVector3f v = b.position, u{q.x, q.y, q.z};
    XrVector3f t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    XrVector3f rv{v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
                  v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
    o.position = {a.position.x + rv.x, a.position.y + rv.y, a.position.z + rv.z};
    return o;
}
XrPosef PoseInv(const XrPosef& a) {
    XrPosef c{};
    c.orientation = {-a.orientation.x, -a.orientation.y, -a.orientation.z, a.orientation.w};
    XrPosef t{}; t.orientation = c.orientation; t.position = {0, 0, 0};
    XrPosef p{}; p.orientation = {0, 0, 0, 1}; p.position = {-a.position.x, -a.position.y, -a.position.z};
    XrPosef r = PoseMul(t, p);
    c.position = r.position;
    return c;
}
XrPosef Identity() { XrPosef p{}; p.orientation.w = 1; return p; }
XrPosef ToXr(const Pose& p) { XrPosef o; o.position = {p.px, p.py, p.pz}; o.orientation = {p.qx, p.qy, p.qz, p.qw}; return o; }
Pose FromXr(const XrPosef& p) { return {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}; }

// ------------------------------------------------------------------------------------------- XR thread state
XrInstance g_inst = XR_NULL_HANDLE;
XrSystemId g_sys = XR_NULL_SYSTEM_ID;
XrSession g_sess = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE, g_view = XR_NULL_HANDLE;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
std::vector<int64_t> g_formats;
XrViewConfigurationView g_vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
XrPosef g_base = Identity();
bool g_needRecenter = true;
uint32_t g_lastRecenter = 0, g_lastDump = 0;

struct Swap {
    XrSwapchain h = XR_NULL_HANDLE;
    uint32_t w = 0, hgt = 0;
    std::vector<XrSwapchainImageD3D11KHR> img;
    std::vector<ID3D11RenderTargetView*> rtv;
    bool srgbView = false;   // RTV (and the matching source SRV) use the _SRGB format: decode+encode = bit-exact copy
};
Swap g_eye[2], g_hud, g_menuSwap;
bool g_menuInit = false, g_menuFailed = false;
XrPosef g_menuPose{};   // world-locked menu: placed in front of the head when it opens
struct Opened { ID3D11Texture2D* tex = nullptr; IDXGIKeyedMutex* mx = nullptr; };
Opened g_open[kSlots];
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
bool g_blitOk = false;
ID3D11ShaderResourceView* g_srv[kSlots][2] = {};   // per shared slot: [0] UNORM view, [1] SRGB view
bool InitBlit();
void ReleaseBlit();
uint32_t g_openGen = 0;
UINT g_eyeW = 0, g_eyeH = 0, g_hudW = 0, g_hudH = 0;
bool g_haveFrame = false;
FrameInfo g_shown{};
uint32_t g_lastSeq = 0;

#define XRCHECK(x)                                                                         \
    do { XrResult _r = (x); if (XR_FAILED(_r)) { LOGE("xr: %s failed: %d", #x, (int)_r); return false; } } while (0)

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool CreateDevice(LUID luid) {
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return false;
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d{};
        ad->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
            LOGI("xr: headset GPU '%ls'", d.Description);
            break;
        }
        Rel(ad);
    }
    f->Release();
    if (!ad) { LOGE("xr: adapter for the headset LUID not found"); return false; }
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    ad->Release();
    if (FAILED(hr)) { LOGE("xr: D3D11CreateDevice 0x%08lx", hr); return false; }
    return true;
}

bool InitInstance() {
    const char* exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy_s(ci.applicationInfo.applicationName, g_set.appName.c_str(), _TRUNCATE);
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "UnityXR-mod");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    XrResult r = xrCreateInstance(&ci, &g_inst);
    if (XR_FAILED(r)) { SetState("no OpenXR runtime (%d)", (int)r); return false; }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_inst, &ip);
    LOGI("xr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
         XR_VERSION_PATCH(ip.runtimeVersion));
    return true;
}

bool InitSystemAndSession() {
    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(g_inst, &sg, &g_sys);
    if (XR_FAILED(r)) { SetState("waiting for headset (%d)", (int)r); return false; }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_inst, g_sys, &sp);
    uint32_t n = 0;
    XRCHECK(xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, g_vcv));
    LOGI("xr: system '%s', recommended per-eye %ux%u (max %ux%u)", sp.systemName, g_vcv[0].recommendedImageRectWidth,
         g_vcv[0].recommendedImageRectHeight, g_vcv[0].maxImageRectWidth, g_vcv[0].maxImageRectHeight);

    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    XRCHECK(xrGetInstanceProcAddr(g_inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XRCHECK(getReq(g_inst, g_sys, &req));
    if (!CreateDevice(req.adapterLuid)) { SetState("no D3D11 device for the headset GPU"); return false; }
    InitBlit();

    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = g_dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g_sys;
    XRCHECK(xrCreateSession(g_inst, &sci, &g_sess));
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace = Identity();
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XRCHECK(xrCreateReferenceSpace(g_sess, &rs, &g_local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XRCHECK(xrCreateReferenceSpace(g_sess, &rs, &g_view));
    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(g_sess, 0, &fc, nullptr);
    g_formats.resize(fc);
    xrEnumerateSwapchainFormats(g_sess, fc, &fc, g_formats.data());
    {
        std::lock_guard<std::mutex> l(S.m);
        S.recW = g_vcv[0].recommendedImageRectWidth;
        S.recH = g_vcv[0].recommendedImageRectHeight;
        S.luid = req.adapterLuid;
    }
    SetState("session created");
    return true;
}

void DestroySwap(Swap& s) {
    for (auto* r : s.rtv) if (r) r->Release();
    if (s.h) xrDestroySwapchain(s.h);
    s = Swap{};
}

bool CreateSwap(Swap& s, uint32_t w, uint32_t h) {
    DestroySwap(s);
    // The game's 8-bit images hold sRGB-encoded values (gamma-space rendering, or an sRGB render target), so they are
    // copied bit-for-bit into an _SRGB swapchain and the runtime decodes them correctly.
    int64_t fmt = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    bool have = false;
    for (auto f : g_formats) have |= f == fmt;
    if (!have) { LOGW("xr: runtime lacks R8G8B8A8_UNORM_SRGB, using its first format %lld", (long long)g_formats[0]); fmt = g_formats[0]; }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XRCHECK(xrCreateSwapchain(g_sess, &ci, &s.h));
    uint32_t n = 0;
    xrEnumerateSwapchainImages(s.h, 0, &n, nullptr);
    s.img.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(s.h, n, &n, (XrSwapchainImageBaseHeader*)s.img.data());
    s.w = w;
    s.hgt = h;
    s.rtv.assign(n, nullptr);
    D3D11_TEXTURE2D_DESC d{};
    if (n) s.img[0].texture->GetDesc(&d);
    s.srgbView = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;   // TYPELESS or UNORM images: raw UNORM views
    return true;
}

void CloseShared() {
    for (auto& a : g_srv) for (auto*& v : a) Rel(v);
    for (auto& o : g_open) { Rel(o.mx); Rel(o.tex); }
    g_openGen = 0;
    g_haveFrame = false;
}

// Opens the game's shared textures and (re)creates the swapchains when the game published a new set.
bool SyncShared() {
    HANDLE h[kSlots];
    UINT ew, eh, hw, hh;
    uint32_t gen;
    {
        std::lock_guard<std::mutex> l(S.m);
        gen = S.gen;
        if (!gen || gen == g_openGen) return g_openGen != 0;
        memcpy(h, S.handles, sizeof(h));
        ew = S.eyeW; eh = S.eyeH; hw = S.hudW; hh = S.hudH;
    }
    CloseShared();
    for (int i = 0; i < kSlots; i++) {
        if (FAILED(g_dev->OpenSharedResource(h[i], __uuidof(ID3D11Texture2D), (void**)&g_open[i].tex)) ||
            FAILED(g_open[i].tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_open[i].mx))) {
            LOGE("xr: cannot open shared slot %d (game and headset on different GPUs?)", i);
            CloseShared();
            return false;
        }
    }
    if (ew != g_eyeW || eh != g_eyeH || !g_eye[0].h) {
        if (!CreateSwap(g_eye[0], ew, eh) || !CreateSwap(g_eye[1], ew, eh)) return false;
        g_eyeW = ew; g_eyeH = eh;
    }
    if (hw != g_hudW || hh != g_hudH || !g_hud.h) {
        if (!CreateSwap(g_hud, hw, hh)) return false;
        g_hudW = hw; g_hudH = hh;
    }
    g_openGen = gen;
    LOGI("xr: opened shared set gen %u (eye %ux%u, hud %ux%u)", gen, ew, eh, hw, hh);
    return true;
}

// ---- copy with optional vertical flip (Unity renders into D3D render textures upside down)
const char kBlitHlsl[] =
    "Texture2D<float4> src : register(t0);\n"
    "cbuffer C : register(b0) { int4 rect; int4 flags; };\n"   // rect = x, y, w, h in the atlas; flags.x = flip Y, .y = flip X
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
    "  int2 q = int2(pos.xy);\n"
    "  if (flags.x != 0) q.y = rect.w - 1 - q.y;\n"
    "  if (flags.y != 0) q.x = rect.z - 1 - q.x;\n"
    "  return src.Load(int3(rect.x + q.x, rect.y + q.y, 0)); }\n";

bool InitBlit() {
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { LOGE("xr: d3dcompiler_47.dll not available - images will not be flipped"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        LOGE("xr: blit shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        if (vsb) vsb->Release();
        return false;
    }
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    vsb->Release(); psb->Release();
    D3D11_BUFFER_DESC bd{32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_rs);
    g_blitOk = g_vs && g_ps && g_cb && g_rs;
    LOGI("xr: flip/copy shader %s", g_blitOk ? "ready" : "FAILED");
    return g_blitOk;
}

void ReleaseBlit() {
    for (auto& a : g_srv) for (auto*& v : a) Rel(v);
    Rel(g_vs); Rel(g_ps); Rel(g_cb); Rel(g_rs);
    g_blitOk = false;
}

ID3D11ShaderResourceView* SlotSrv(int slot, ID3D11Texture2D* tex, bool srgb) {
    ID3D11ShaderResourceView*& v = g_srv[slot][srgb ? 1 : 0];
    if (!v) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(g_dev->CreateShaderResourceView(tex, &sd, &v))) { LOGE("xr: SRV on shared slot %d failed", slot); v = nullptr; }
    }
    return v;
}

void CopyToSwap(Swap& s, int slot, ID3D11Texture2D* src, UINT x, UINT y, UINT w, UINT h, bool flipY, bool flipX) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(s.h, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(s.h, &wi))) {
        ID3D11ShaderResourceView* srv = g_blitOk ? SlotSrv(slot, src, s.srgbView) : nullptr;
        if (srv && !s.rtv[idx]) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = s.srgbView ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(g_dev->CreateRenderTargetView(s.img[idx].texture, &rd, &s.rtv[idx]))) s.rtv[idx] = nullptr;
        }
        if (srv && s.rtv[idx]) {
            int cb[8] = {(int)x, (int)y, (int)w, (int)h, flipY ? 1 : 0, flipX ? 1 : 0, 0, 0};
            g_ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
            D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
            g_ctx->OMSetRenderTargets(1, &s.rtv[idx], nullptr);
            g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            g_ctx->OMSetDepthStencilState(nullptr, 0);
            g_ctx->RSSetViewports(1, &vp);
            g_ctx->RSSetState(g_rs);
            g_ctx->IASetInputLayout(nullptr);
            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_ctx->VSSetShader(g_vs, nullptr, 0);
            g_ctx->PSSetShader(g_ps, nullptr, 0);
            g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
            g_ctx->PSSetShaderResources(0, 1, &srv);
            g_ctx->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr;
            g_ctx->PSSetShaderResources(0, 1, &none);
            g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        } else {
            D3D11_BOX b{x, y, 0, x + w, y + h, 1};
            g_ctx->CopySubresourceRegion(s.img[idx].texture, 0, 0, 0, 0, src, 0, &b);
        }
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(s.h, &ri);
}

bool WriteBmp(const wchar_t* path, const uint8_t* px, unsigned w, unsigned h, unsigned pitch, bool flipY) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih); ih.biWidth = (LONG)w; ih.biHeight = -(LONG)h; ih.biPlanes = 1; ih.biBitCount = 32;
    ih.biCompression = BI_RGB; ih.biSizeImage = w * h * 4;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> row(w * 4);
    for (unsigned y = 0; y < h; y++) {
        const uint8_t* s = px + (size_t)(flipY ? h - 1 - y : y) * pitch;
        for (unsigned x = 0; x < w; x++) { row[x * 4] = s[x * 4 + 2]; row[x * 4 + 1] = s[x * 4 + 1]; row[x * 4 + 2] = s[x * 4]; row[x * 4 + 3] = s[x * 4 + 3]; }
        fwrite(row.data(), row.size(), 1, f);
    }
    fclose(f);
    return true;
}

// Saves the slot's eye and HUD regions (call while holding the slot's mutex).
void DumpSlot(ID3D11Texture2D* atlas, uint32_t id) {
    D3D11_TEXTURE2D_DESC d{};
    atlas->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st))) return;
    g_ctx->CopyResource(st, atlas);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const uint8_t* p = (const uint8_t*)m.pData;
        struct { const wchar_t* n; UINT x, y, w, h; bool flip; } parts[] = {
            {L"left", 0, 0, g_eyeW, g_eyeH, g_set.flipEyesY}, {L"right", g_eyeW, 0, g_eyeW, g_eyeH, g_set.flipEyesY},
            {L"hud", 0, g_eyeH, g_hudW, g_hudH, g_set.flipHudY}};
        for (auto& pt : parts) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%s\\neovr_dump_%u_%s.bmp", g_set.dumpDir.c_str(), id, pt.n);
            bool ok = WriteBmp(path, p + (size_t)pt.y * m.RowPitch + pt.x * 4, pt.w, pt.h, m.RowPitch, pt.flip);
            LOGI("xr: dump %ls %ux%u %s", path, pt.w, pt.h, ok ? "saved" : "FAILED");
        }
        g_ctx->Unmap(st, 0);
    }
    st->Release();
}

// Copies the newest game frame (if there is a new one) into the swapchains.
void FetchFrame() {
    if (!SyncShared()) return;
    int slot;
    uint32_t seq, dumpReq;
    {
        std::lock_guard<std::mutex> l(S.m);
        slot = S.latest;
        seq = S.submitSeq;
        dumpReq = S.dumpReq;
    }
    if (slot < 0 || seq == g_lastSeq) return;
    if (g_open[slot].mx->AcquireSync(1, 2) != S_OK) return;     // the game took it back for a newer frame
    if (g_meta[slot].gen == g_openGen) {
        g_shown = g_meta[slot].fi;
        ID3D11Texture2D* t = g_open[slot].tex;
        CopyToSwap(g_eye[0], slot, t, 0, 0, g_eyeW, g_eyeH, g_set.flipEyesY, g_set.flipEyesX);
        CopyToSwap(g_eye[1], slot, t, g_eyeW, 0, g_eyeW, g_eyeH, g_set.flipEyesY, g_set.flipEyesX);
        CopyToSwap(g_hud, slot, t, 0, g_eyeH, g_hudW, g_hudH, g_set.flipHudY, g_set.flipHudX);
        if (dumpReq != g_lastDump) { g_lastDump = dumpReq; DumpSlot(t, dumpReq); }
        g_haveFrame = true;
        g_lastSeq = seq;
        st_xrNew++;
    }
    g_open[slot].mx->ReleaseSync(0);
}

bool DoRecenter(XrTime t) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_view, g_local, t, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    const XrQuaternionf& q = loc.pose.orientation;
    // forward = q * (0,0,-1)
    float fx = -(2 * (q.x * q.z + q.w * q.y)), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    float yaw = atan2f(-fx, -fz);
    g_base.orientation = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    g_base.position = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ? loc.pose.position : XrVector3f{0, 0, 0};
    LOGI("xr: recentred (yaw %.1f deg, pos %.2f %.2f %.2f)", yaw * 57.2958f, g_base.position.x, g_base.position.y, g_base.position.z);
    return true;
}

void HandleEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* e = (XrEventDataSessionStateChanged*)&ev;
            g_state = e->state;
            LOGI("xr: session state %d", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_sess, &bi))) { g_running = true; g_needRecenter = true; SetState("running"); }
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_sess);
                g_running = false;
                SetState("stopped");
            } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = false;
                SetState(g_state == XR_SESSION_STATE_EXITING ? "exiting" : "session lost");
            } else if (g_state == XR_SESSION_STATE_FOCUSED) {
                SetState("focused");
            } else if (g_state == XR_SESSION_STATE_VISIBLE) {
                SetState("visible");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            g_needRecenter = true;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g_running = false;
            SetState("instance lost");
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Settings menu (vrkit) on the XR thread: draws with our device; returns true and fills the quad while open.
bool UpdateSettingsMenu(XrCompositionLayerQuad& q, XrTime t) {
    if (!g_set.settingsMenu || !g_menuAllowed || g_menuFailed) return false;
    if (!g_menuInit) {
        vrkit::menu::Settings ms;
        ms.title = g_set.menuTitle;
        ms.configPath = g_set.configPath;
        ms.statusLine = g_set.menuStatus;
        if (!vrkit::menu::Init(g_dev, g_ctx, ms) || !CreateSwap(g_menuSwap, vrkit::menu::Width(), vrkit::menu::Height())) {
            LOGE("xr: settings menu unavailable");
            g_menuFailed = true;
            return false;
        }
        g_menuInit = true;
        LOGI("xr: settings menu ready (hold L3+R3 or press Insert)");
    }
    bool open = vrkit::menu::Update();
    if (open != g_menuOpen.load()) {
        LOGI("xr: settings menu %s", open ? "opened" : "closed");
        if (open) {
            // Anchor it in the room in front of where the head points now (yaw only, so it stays upright).
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            XrPosef head = Identity();
            if (XR_SUCCEEDED(xrLocateSpace(g_view, g_local, t, &loc)) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                const XrQuaternionf& o = loc.pose.orientation;
                float fx = -(2 * (o.x * o.z + o.w * o.y)), fz = -(1 - 2 * (o.x * o.x + o.y * o.y));
                float yaw = atan2f(-fx, -fz);
                head.orientation = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
                if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) head.position = loc.pose.position;
            }
            XrPosef off = Identity();
            off.position = {0, 0, -g_set.settingsMenuDistance};
            g_menuPose = PoseMul(head, off);
        }
    }
    g_menuOpen = open;
    if (!open) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(g_menuSwap.h, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(g_menuSwap.h, &wi))) g_ctx->CopyResource(g_menuSwap.img[idx].texture, vrkit::menu::Texture());
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_menuSwap.h, &ri);
    float w = g_set.settingsMenuSize, h = w * vrkit::menu::Height() / (float)vrkit::menu::Width();
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied
    if (g_set.settingsMenuFollowHead) {
        q.space = g_view;
        q.pose = Identity();
        q.pose.position = {0, 0, -g_set.settingsMenuDistance};
    } else {
        q.space = g_local;   // fixed in the room
        q.pose = g_menuPose;
    }
    q.subImage.swapchain = g_menuSwap.h;
    q.subImage.imageRect = {{0, 0}, {(int32_t)g_menuSwap.w, (int32_t)g_menuSwap.hgt}};
    q.size = {w, h};
    return true;
}

void Frame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    double tw0 = perf::NowMs();
    if (XR_FAILED(xrWaitFrame(g_sess, &wi, &fs))) { Sleep(10); return; }
    double tw1 = perf::NowMs();
    g_beat++;
    SetEvent(g_beatEvt);
    {
        std::lock_guard<std::mutex> l(g_pm);
        p_wait.add(tw1 - tw0);
        p_periodMs = fs.predictedDisplayPeriod / 1e6;
        if (p_lastWaitDone) {
            double iv = tw1 - p_lastWaitDone;
            p_interval.add(iv);
            if (p_periodMs > 0 && iv > 1.5 * p_periodMs) p_late++;
        }
        p_lastWaitDone = tw1;
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(g_sess, &bi))) return;
    st_xrFrames++;

    uint32_t rc;
    {
        std::lock_guard<std::mutex> l(S.m);
        rc = S.recenterReq;
    }
    if (rc != g_lastRecenter || g_needRecenter) { if (DoRecenter(fs.predictedDisplayTime)) { g_lastRecenter = rc; g_needRecenter = false; } }

    // Poses for the game (predicted for this display time; the game renders roughly one headset frame later).
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime + fs.predictedDisplayPeriod;
    vli.space = g_local;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t vc = 0;
    if (XR_SUCCEEDED(xrLocateViews(g_sess, &vli, &vs, 2, &vc, views)) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        XrPosef inv = PoseInv(g_base);
        std::lock_guard<std::mutex> l(S.m);
        for (int e = 0; e < 2; e++) {
            S.pose[e] = FromXr(PoseMul(inv, views[e].pose));
            S.fov[e] = {views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp, views[e].fov.angleDown};
        }
        S.time = vli.displayTime;
        S.ready = true;
    }

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (fs.shouldRender) {
        uint32_t before = st_xrNew;
        double tc0 = perf::NowMs();
        FetchFrame();
        if (st_xrNew != before) { std::lock_guard<std::mutex> l(g_pm); p_copy.add(perf::NowMs() - tc0); }
        if (!g_haveFrame) st_xrNone++;
    }
    if (fs.shouldRender && g_haveFrame) {
        float hd, hwid, hy, md, mw;
        bool follow;
        {
            std::lock_guard<std::mutex> l(S.m);
            hd = S.hudDistance; hwid = S.hudWidth; hy = S.hudOffsetY; md = S.menuDistance; mw = S.menuWidth; follow = S.hudFollowHead;
        }
        bool menu = (g_shown.flags & FRAME_MENU_MODE) != 0;
        if (!menu) {
            for (int e = 0; e < 2; e++) {
                pv[e].pose = PoseMul(g_base, ToXr(g_shown.pose[e]));   // the pose this image was rendered with
                pv[e].fov = {g_shown.fov[e].left, g_shown.fov[e].right, g_shown.fov[e].up, g_shown.fov[e].down};
                pv[e].subImage.swapchain = g_eye[e].h;
                pv[e].subImage.imageRect = {{0, 0}, {(int32_t)g_eyeW, (int32_t)g_eyeH}};
            }
            proj.space = g_local;
            proj.viewCount = 2;
            proj.views = pv;
            layers.push_back((XrCompositionLayerBaseHeader*)&proj);
        }
        if (menu || (g_shown.flags & FRAME_HUD_VISIBLE)) {
            float d = menu ? md : hd, w = menu ? mw : hwid;
            quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied
            quad.subImage.swapchain = g_hud.h;
            quad.subImage.imageRect = {{0, 0}, {(int32_t)g_hudW, (int32_t)g_hudH}};
            quad.size = {w, w * g_hudH / (float)g_hudW};
            XrPosef local = Identity();
            local.position = {0, menu ? 0.f : hy, -d};
            if (follow && !menu) { quad.space = g_view; quad.pose = local; }
            else { quad.space = g_local; quad.pose = PoseMul(g_base, local); }
            layers.push_back((XrCompositionLayerBaseHeader*)&quad);
        }
    }
    XrCompositionLayerQuad menuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (fs.shouldRender && UpdateSettingsMenu(menuQuad, fs.predictedDisplayTime)) layers.push_back((XrCompositionLayerBaseHeader*)&menuQuad);   // on top
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = (uint32_t)layers.size();
    ei.layers = layers.data();
    double te0 = perf::NowMs();
    XrResult r = xrEndFrame(g_sess, &ei);
    { std::lock_guard<std::mutex> l(g_pm); p_end.add(perf::NowMs() - te0); }
    if (XR_FAILED(r)) LOGW("xr: xrEndFrame %d", (int)r);
}

void TearDown() {
    CloseShared();
    DestroySwap(g_eye[0]); DestroySwap(g_eye[1]); DestroySwap(g_hud); DestroySwap(g_menuSwap);
    if (g_menuInit) { vrkit::menu::Shutdown(); g_menuInit = false; }
    g_menuOpen = false;
    g_eyeW = g_eyeH = g_hudW = g_hudH = 0;
    if (g_view) xrDestroySpace(g_view);
    if (g_local) xrDestroySpace(g_local);
    if (g_sess) xrDestroySession(g_sess);
    g_view = g_local = XR_NULL_HANDLE;
    g_sess = XR_NULL_HANDLE;
    ReleaseBlit();
    Rel(g_ctx); Rel(g_dev);
    g_running = false;
    std::lock_guard<std::mutex> l(S.m);
    S.ready = false;
}

DWORD WINAPI XrThread(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (!g_quit) {
        if (!g_inst && !InitInstance()) { Sleep(3000); continue; }
        if (!g_sess) {
            if (!InitSystemAndSession()) { TearDown(); Sleep(2000); continue; }
        }
        HandleEvents();
        if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
            TearDown();
            g_state = XR_SESSION_STATE_UNKNOWN;
            if (g_inst) { xrDestroyInstance(g_inst); g_inst = XR_NULL_HANDLE; }
            Sleep(2000);
            continue;
        }
        if (g_running) Frame();
        else Sleep(20);
    }
    if (g_running) xrRequestExitSession(g_sess);
    TearDown();
    if (g_inst) xrDestroyInstance(g_inst);
    return 0;
}

// ------------------------------------------------------------------------------------------- game side (render thread)
ID3D11Device* g_gdev = nullptr;
ID3D11DeviceContext* g_gctx = nullptr;
struct GSlot { ID3D11Texture2D* tex = nullptr; IDXGIKeyedMutex* mx = nullptr; };
GSlot g_gs[kSlots];
UINT g_gEyeW = 0, g_gEyeH = 0, g_gHudW = 0, g_gHudH = 0;
DXGI_FORMAT g_gFmt = DXGI_FORMAT_UNKNOWN;
uint32_t g_gGen = 0;
int g_gNext = 0;

DXGI_FORMAT AtlasFormat(DXGI_FORMAT f) {
    switch (f) {   // any member of the R8G8B8A8 family: the copy is bit-exact, the XR side decides how to read it
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;   // typeless so the XR side can view it as UNORM or SRGB
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool EnsureGameSet(UINT ew, UINT eh, UINT hw, UINT hh, DXGI_FORMAT fmt) {
    if (g_gs[0].tex && ew == g_gEyeW && eh == g_gEyeH && hw == g_gHudW && hh == g_gHudH && fmt == g_gFmt) return true;
    for (auto& s : g_gs) { Rel(s.mx); Rel(s.tex); }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = ew * 2 > hw ? ew * 2 : hw;
    d.Height = eh + hh;
    d.MipLevels = d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    HANDLE hs[kSlots]{};
    for (int i = 0; i < kSlots; i++) {
        HRESULT hr = g_gdev->CreateTexture2D(&d, nullptr, &g_gs[i].tex);
        IDXGIResource* r = nullptr;
        if (SUCCEEDED(hr)) hr = g_gs[i].tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_gs[i].mx);
        if (SUCCEEDED(hr)) hr = g_gs[i].tex->QueryInterface(__uuidof(IDXGIResource), (void**)&r);
        if (SUCCEEDED(hr)) { hr = r->GetSharedHandle(&hs[i]); r->Release(); }
        if (FAILED(hr)) { LOGE("xr: shared texture %ux%u fmt %d failed 0x%08lx", d.Width, d.Height, fmt, hr); return false; }
    }
    g_gEyeW = ew; g_gEyeH = eh; g_gHudW = hw; g_gHudH = hh; g_gFmt = fmt;
    std::lock_guard<std::mutex> l(S.m);
    g_gGen = ++S.gen;
    memcpy(S.handles, hs, sizeof(hs));
    S.eyeW = ew; S.eyeH = eh; S.hudW = hw; S.hudH = hh; S.atlasW = d.Width; S.atlasH = d.Height; S.fmt = fmt;
    S.latest = -1;
    st_recreated++;
    LOGI("xr: game shared set gen %u: eye %ux%u, hud %ux%u, atlas %ux%u", g_gGen, ew, eh, hw, hh, d.Width, d.Height);
    return true;
}

} // namespace

// ------------------------------------------------------------------------------------------- API
bool Init(const Settings& s) {
    g_set = s;
    SetHud(s.hudDistance, s.hudWidth, s.hudOffsetY, s.hudFollowHead);
    {
        std::lock_guard<std::mutex> l(S.m);
        S.menuDistance = s.menuDistance;
        S.menuWidth = s.menuWidth;
    }
    g_thread = CreateThread(nullptr, 0, XrThread, nullptr, 0, nullptr);
    return g_thread != nullptr;
}

void Shutdown() {
    g_quit = true;
    if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); g_thread = nullptr; }
}

bool Ready() { std::lock_guard<std::mutex> l(S.m); return S.ready; }

bool LatestPoses(Pose pose[2], Fov fov[2], int64_t* time) {
    std::lock_guard<std::mutex> l(S.m);
    if (!S.ready) return false;
    memcpy(pose, S.pose, sizeof(S.pose));
    memcpy(fov, S.fov, sizeof(S.fov));
    *time = S.time;
    return true;
}

bool RecommendedSize(unsigned* w, unsigned* h) {
    std::lock_guard<std::mutex> l(S.m);
    if (!S.recW) return false;
    *w = S.recW; *h = S.recH;
    return true;
}

void SetMenuPanel(float d, float w) { std::lock_guard<std::mutex> l(S.m); S.menuDistance = d; S.menuWidth = w; }
void EnableSettingsMenu() { g_menuAllowed = true; }
bool SettingsMenuOpen() { return g_menuOpen.load(); }

void Recenter() { std::lock_guard<std::mutex> l(S.m); S.recenterReq++; }
void RequestDump(uint32_t id) { std::lock_guard<std::mutex> l(S.m); S.dumpReq = id; }
void SetHud(float d, float w, float y, bool f) {
    std::lock_guard<std::mutex> l(S.m);
    S.hudDistance = d; S.hudWidth = w; S.hudOffsetY = y; S.hudFollowHead = f;
}

void SubmitRenderThread(ID3D11Texture2D* left, ID3D11Texture2D* right, ID3D11Texture2D* hud, DXGI_FORMAT viewFormat,
                        const FrameInfo& fi) {
    if (!left || !right) return;
    if (!g_gdev) {
        left->GetDevice(&g_gdev);
        g_gdev->GetImmediateContext(&g_gctx);
        IDXGIDevice* dx = nullptr;
        if (SUCCEEDED(g_gdev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx))) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dx->GetAdapter(&ad))) {
                DXGI_ADAPTER_DESC d{};
                ad->GetDesc(&d);
                LUID hl;
                { std::lock_guard<std::mutex> l(S.m); hl = S.luid; }
                LOGI("xr: game GPU '%ls' (%s the headset GPU)", d.Description,
                     d.AdapterLuid.LowPart == hl.LowPart && d.AdapterLuid.HighPart == hl.HighPart ? "same as" : "NOT");
                ad->Release();
            }
            dx->Release();
        }
    }
    D3D11_TEXTURE2D_DESC ed{}, hd{};
    left->GetDesc(&ed);
    if (hud) hud->GetDesc(&hd);
    DXGI_FORMAT fmt = AtlasFormat(viewFormat != DXGI_FORMAT_UNKNOWN ? viewFormat : ed.Format);
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        static bool once = false;
        if (!once) { once = true; LOGE("xr: unsupported eye texture format %d", ed.Format); }
        return;
    }
    if (!EnsureGameSet(ed.Width, ed.Height, hud ? hd.Width : 16, hud ? hd.Height : 16, fmt)) return;

    int latest;
    { std::lock_guard<std::mutex> l(S.m); latest = S.latest; }
    int slot = -1;
    for (int k = 0; k < kSlots && slot < 0; k++) {
        int s = (g_gNext + k) % kSlots;
        if (s == latest) continue;
        // key 0: free (the XR side consumed it); key 1: submitted but superseded before the XR side took it
        if (g_gs[s].mx->AcquireSync(0, 0) == S_OK || g_gs[s].mx->AcquireSync(1, 0) == S_OK) slot = s;
    }
    if (slot < 0) { st_noSlot++; return; }
    g_gNext = (slot + 1) % kSlots;
    ID3D11Texture2D* dst = g_gs[slot].tex;
    g_gctx->CopySubresourceRegion(dst, 0, 0, 0, 0, left, 0, nullptr);
    g_gctx->CopySubresourceRegion(dst, 0, g_gEyeW, 0, 0, right, 0, nullptr);
    if (hud) g_gctx->CopySubresourceRegion(dst, 0, 0, g_gEyeH, 0, hud, 0, nullptr);
    g_meta[slot].fi = fi;
    g_meta[slot].gen = g_gGen;
    g_gs[slot].mx->ReleaseSync(1);
    {
        std::lock_guard<std::mutex> l(S.m);
        S.latest = slot;
        S.submitSeq++;
    }
    st_submitted++;
}

double WaitForHeadsetFrame(DWORD timeoutMs) {
    static uint32_t seen = 0;
    double t0 = perf::NowMs();
    while (g_beat.load() == seen) {
        double left = timeoutMs - (perf::NowMs() - t0);
        if (left <= 0) break;
        WaitForSingleObject(g_beatEvt, (DWORD)left + 1);
    }
    seen = g_beat.load();
    return perf::NowMs() - t0;
}

XrPerf TakeXrPerf() {
    std::lock_guard<std::mutex> l(g_pm);
    XrPerf p{p_wait.avg(), p_interval.avg(), p_interval.max, p_copy.avg(), p_copy.max, p_end.avg(), p_interval.n, p_late, p_periodMs};
    p_wait.reset(); p_interval.reset(); p_copy.reset(); p_end.reset(); p_late = 0;
    return p;
}

Stats TakeStats() {
    return {st_submitted.exchange(0), st_noSlot.exchange(0), st_recreated.exchange(0), st_xrFrames.exchange(0),
            st_xrNew.exchange(0), st_xrNone.exchange(0)};
}

const char* StateName() {
    static char buf[96];
    std::lock_guard<std::mutex> l(S.m);
    strcpy_s(buf, S.state);
    return buf;
}

} // namespace uxr::xrout
