#include "stereo.h"
#include "xrout.h"
#include "perf.h"
#include "vrkit_options.h"
#include <atomic>
#include "../unity/unity.h"
#include "../common/log.h"
#include <d3d11.h>
#include <cmath>
#include <vector>
#include <sstream>

namespace uxr::stereo {
using namespace uxr::unity;

namespace {

struct Cfg {
    bool vr = true;
    std::vector<std::string> worldCams, hudCams;
    float renderScale = 1.0f;         // used when renderPreset = custom
    std::string renderPreset = "native";
    float hudDistance = 1.6f, hudSize = 1.6f, hudOffsetY = -0.05f, menuDistance = 2.0f, menuSize = 2.4f;
    bool hudFollowHead = false;
    int eyeW = 0, eyeH = 0;          // 0 = from headset
    int hudW = 1920, hudH = 1080;
    float worldScale = 1.0f;          // game units per metre of head motion
    bool headTracking = true;
    bool levelCamera = false;         // drop the game camera's pitch/roll, keep yaw
    bool hudInHeadset = true;
    int keyRecenter = VK_F9, keyHud = VK_F10, keyDump = VK_F11;
    float debugDumpAtSec = 0;
    bool pacing = true;          // render at most one game frame per headset frame
    int pacingTimeoutMs = 30;
    bool desktopWorld = true;    // let the game also render the world camera to the monitor
    bool perfLog = false;        // one performance line per second
    float perfSlowMs = 0;        // >0: only log seconds whose slowest game frame exceeded this
} C;

// ---- render resolution presets (per-eye size = headset recommended size x scale)
struct Preset { const char* name; const char* label; float scale; };
const Preset kPresets[] = {
    {"performance", "Performance (70%)", 0.70f}, {"balanced", "Balanced (85%)", 0.85f}, {"native", "Native (100%)", 1.00f},
    {"high", "High (120%)", 1.20f}, {"ultra", "Ultra (140%)", 1.40f}, {"custom", "Custom (render_scale)", -1.f},
};
std::atomic<float> g_effScale{1.0f};   // written by the menu (XR thread) and Init; read by the main thread

void ApplyRenderPreset() {
    float s = C.renderScale;
    for (auto& p : kPresets)
        if (C.renderPreset == p.name && p.scale > 0) s = p.scale;
    if (s < 0.25f) s = 0.25f;
    if (s > 2.0f) s = 2.0f;
    if (s != g_effScale.load()) LOGI("stereo: render scale -> %.2f (preset '%s')", s, C.renderPreset.c_str());
    g_effScale = s;
}

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
    }
    return out;
}

// --- state (main thread)
Handle g_eyeRT[2], g_hudRT;
ID3D11Texture2D* g_eyeTex[2] = {};
ID3D11Texture2D* g_hudTex = nullptr;
int g_eyeW = 0, g_eyeH = 0;
Handle g_worldCam;
std::string g_worldName;
std::vector<Handle> g_hudCams;
std::vector<Object*> g_renderedThisFrame, g_renderedLastFrame;
bool g_inOwnRender = false;
bool g_hudVisible = true;
DWORD g_lastScan = 0;
bool g_keyState[256] = {};
DWORD g_firstVrTick = 0;
bool g_dumped = false;
uint32_t g_dumpId = 0;
DXGI_FORMAT g_viewFormat = DXGI_FORMAT_UNKNOWN;

// stats
uint32_t s_frames = 0, s_stereo = 0, s_menu = 0, s_noPose = 0;
DWORD s_lastReport = 0;
// per-second performance
double p_lastFrame = 0, p_lastLog = 0;
perf::Acc p_frame, p_pace, p_stereoCpu;
uint32_t p_frames = 0, p_gameRenders = 0, p_skippedRenders = 0;
std::atomic<float> g_statGameFps{0}, g_statXrFps{0}, g_statGpuMs{0};

// --- frames handed to the render thread (GL.IssuePluginEvent carries only an int)
constexpr int kRing = 16;
xrout::FrameInfo g_ring[kRing];
int g_ringIdx = 0;

struct RingTex { ID3D11Texture2D *eye[2], *hud; };
RingTex g_ringTex[kRing];

void __stdcall RenderThreadSubmit(int id) {
    const xrout::FrameInfo& fi = g_ring[id & (kRing - 1)];
    const RingTex& t = g_ringTex[id & (kRing - 1)];   // the textures this frame was rendered into (survive a resize)
    xrout::SubmitRenderThread(t.eye[0], t.eye[1], t.hud, g_viewFormat, fi);
}

bool KeyPressed(int vk) {
    if (vk <= 0 || vk > 255) return false;
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !g_keyState[vk];
    g_keyState[vk] = down;
    return edge;
}

// ---- math (Unity: left-handed, +Z forward, +Y up)
Quat QMul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Vec3 QRot(const Quat& q, const Vec3& v) {
    // v' = v + 2w(u x v) + 2 u x (u x v)
    Vec3 u{q.x, q.y, q.z};
    Vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// OpenXR (right-handed, -Z forward) -> Unity (left-handed, +Z forward): mirror Z.
Vec3 XrPosToUnity(const xrout::Pose& p) { return {p.px, p.py, -p.pz}; }
Quat XrRotToUnity(const xrout::Pose& p) { return {-p.qx, -p.qy, p.qz, p.qw}; }

Quat YawOnly(const Quat& q) {
    Vec3 f = QRot(q, {0, 0, 1});
    float yaw = atan2f(f.x, f.z);
    return {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
}

// Unity worldToCameraMatrix: camera space is right-handed (-Z forward), i.e. view = scale(1,1,-1) * inverse(TRS).
Matrix4x4 ViewMatrix(const Vec3& pos, const Quat& rot) {
    Vec3 r = QRot(rot, {1, 0, 0}), u = QRot(rot, {0, 1, 0}), f = QRot(rot, {0, 0, 1});
    Matrix4x4 m{};
    m.at(0, 0) = r.x; m.at(0, 1) = r.y; m.at(0, 2) = r.z; m.at(0, 3) = -Dot(r, pos);
    m.at(1, 0) = u.x; m.at(1, 1) = u.y; m.at(1, 2) = u.z; m.at(1, 3) = -Dot(u, pos);
    m.at(2, 0) = -f.x; m.at(2, 1) = -f.y; m.at(2, 2) = -f.z; m.at(2, 3) = Dot(f, pos);
    m.at(3, 3) = 1;
    return m;
}

// OpenGL-convention off-axis projection (Unity converts it for D3D itself).
Matrix4x4 ProjMatrix(const xrout::Fov& fov, float n, float f) {
    float l = tanf(fov.left), r = tanf(fov.right), t = tanf(fov.up), b = tanf(fov.down);
    Matrix4x4 m{};
    m.at(0, 0) = 2 / (r - l);
    m.at(0, 2) = (r + l) / (r - l);
    m.at(1, 1) = 2 / (t - b);
    m.at(1, 2) = (t + b) / (t - b);
    m.at(2, 2) = -(f + n) / (f - n);
    m.at(2, 3) = -2 * f * n / (f - n);
    m.at(3, 2) = -1;
    return m;
}

bool Alive(Object* o) {
    // A destroyed UnityEngine.Object keeps its managed shell; m_CachedPtr (first field after the header) becomes null.
    return o && *(void**)((char*)o + 16) != nullptr;
}

ID3D11Texture2D* NativeTex(Object* rt) { return rt ? (ID3D11Texture2D*)I.Texture_GetNativeTexturePtr(rt) : nullptr; }

bool EnsureTargets() {
    unsigned rw = 0, rh = 0;
    int w = C.eyeW, h = C.eyeH;
    if (w <= 0 || h <= 0) {
        if (!xrout::RecommendedSize(&rw, &rh) || !rw) return false;
        float sc = g_effScale.load();
        w = (int)(rw * sc);
        h = (int)(rh * sc);
    }
    w = (w + 1) & ~1;
    h = (h + 1) & ~1;
    if (g_eyeRT[0].get() && Alive(g_eyeRT[0].get()) && w == g_eyeW && h == g_eyeH) return true;
    for (int e = 0; e < 2; e++) {
        // Free the old target's GPU memory (queued on the render thread after any frame still using it).
        if (Object* old = g_eyeRT[e].get()) if (Alive(old) && I.RenderTexture_Release) I.RenderTexture_Release(old);
        Object* rt = NewRenderTexture(w, h, 24, 0 /* ARGB32 */, e ? "NeoVR_EyeR" : "NeoVR_EyeL");
        if (!rt) return false;
        g_eyeRT[e].set(rt);
        g_eyeTex[e] = NativeTex(rt);
    }
    if (!g_hudRT.get() || !Alive(g_hudRT.get())) {
        Object* rt = NewRenderTexture(C.hudW, C.hudH, 24, 0, "NeoVR_HUD");
        if (!rt) return false;
        g_hudRT.set(rt);
        g_hudTex = NativeTex(rt);
    }
    g_eyeW = w;
    g_eyeH = h;
    if (g_eyeTex[0]) {
        D3D11_TEXTURE2D_DESC d{};
        g_eyeTex[0]->GetDesc(&d);
        int cs = 0;
        InvokeValue(M.QualitySettings_get_activeColorSpace, nullptr, cs);
        // ARGB32 is R8G8B8A8_TYPELESS on D3D11; Unity writes gamma values in Gamma space, linear->sRGB in Linear space.
        if (d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) g_viewFormat = cs == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        else g_viewFormat = DXGI_FORMAT_UNKNOWN;
        LOGI("stereo: eye targets %dx%d (headset %ux%u x %.2f), native format %d, colour space %s -> view format %d", w, h,
             rw, rh, g_effScale.load(), d.Format, cs == 1 ? "Linear" : "Gamma", g_viewFormat);
    }
    return g_eyeTex[0] && g_eyeTex[1];
}

void ScanCameras() {
    auto cams = FindObjectsOfType(M.Camera);
    // World camera: first configured name that actually rendered last frame (to the screen); else first enabled match.
    Object* best = nullptr;
    std::string bestName;
    for (auto& want : C.worldCams) {
        for (Object* c : cams) {
            if (Name(c) != want) continue;
            if (I.Camera_get_targetTexture(c)) continue;
            bool rendered = false;
            for (Object* r : g_renderedLastFrame) rendered |= (r == c);
            bool act = false;
            InvokeValue(M.Behaviour_get_isActiveAndEnabled, c, act);
            if (rendered || act) { best = c; bestName = want; break; }
        }
        if (best) break;
    }
    if (best != g_worldCam.get()) {
        LOGI("stereo: world camera -> '%s' [%p]", best ? bestName.c_str() : "<none>", best);
        g_worldCam.set(best);
        g_worldName = bestName;
    }
    // HUD cameras: redirect into the HUD texture (they keep it; re-applied if the game resets it).
    std::vector<Handle> hud;
    for (Object* c : cams) {
        std::string n = Name(c);
        for (auto& want : C.hudCams) {
            if (n != want) continue;
            Object* tt = I.Camera_get_targetTexture(c);
            if (C.hudInHeadset && tt != g_hudRT.get()) {
                if (tt) { LOGW("stereo: HUD camera '%s' already renders to a texture; left alone", n.c_str()); break; }
                I.Camera_set_targetTexture(c, g_hudRT.get());
                LOGI("stereo: HUD camera '%s' -> HUD texture", n.c_str());
            }
            Handle h; h.set(c);
            hud.push_back(h);
            break;
        }
    }
    for (auto& h : g_hudCams) h.reset();
    g_hudCams = hud;
}

void ClearHud() {
    Object* prev = I.RenderTexture_GetActive();
    I.RenderTexture_SetActive(g_hudRT.get());
    Color c{0, 0, 0, 0};
    I.GL_Clear(true, true, &c, 1.0f);
    I.RenderTexture_SetActive(prev);
}

void RenderEye(Object* cam, int eye, const Vec3& gp, const Quat& gr, const xrout::Pose& pose, const xrout::Fov& fov,
               float n, float f) {
    Vec3 hp = XrPosToUnity(pose);
    Quat hr = XrRotToUnity(pose);
    if (!C.headTracking) { hp = {hp.x, 0, 0}; hr = {0, 0, 0, 1}; }  // keep only the eye separation
    Vec3 off = QRot(gr, {hp.x * C.worldScale, hp.y * C.worldScale, hp.z * C.worldScale});
    Vec3 pos{gp.x + off.x, gp.y + off.y, gp.z + off.z};
    Quat rot = QMul(gr, hr);
    Matrix4x4 view = ViewMatrix(pos, rot);
    Matrix4x4 proj = ProjMatrix(fov, n, f);
    I.Camera_set_targetTexture(cam, g_eyeRT[eye].get());
    I.Camera_set_worldToCameraMatrix(cam, &view);
    I.Camera_set_projectionMatrix(cam, &proj);
    I.Camera_Render(cam);
}

void PerfSecond() {
    double now = perf::NowMs();
    if (!p_lastLog) { p_lastLog = now; return; }
    if (now - p_lastLog < 1000) return;
    double secs = (now - p_lastLog) / 1000.0;
    p_lastLog = now;
    auto x = xrout::TakeXrPerf();
    auto g = perf::TakeGpu();
    g_statGameFps = (float)(p_frames / secs);
    g_statXrFps = (float)(x.frames / secs);
    g_statGpuMs = (float)g.stereo.avg();
    if (C.perfLog && (C.perfSlowMs <= 0 || p_frame.max > C.perfSlowMs || x.late > 0)) {
        LOGI("perf: %s | game %.0f fps, frame avg %.1f max %.1f ms, pacing wait %.1f ms, stereo cpu %.1f/%.1f ms | "
             "gpu stereo %.1f/%.1f ms, gpu frame %.1f/%.1f ms | xr %.0f fps, late %u, interval max %.1f ms, wait %.1f, copy %.1f/%.1f, end %.1f | "
             "desktop renders %u skipped %u",
             ActiveSceneName().c_str(), p_frames / secs, p_frame.avg(), p_frame.max, p_pace.avg(), p_stereoCpu.avg(), p_stereoCpu.max,
             g.stereo.avg(), g.stereo.max, g.frame.avg(), g.frame.max, x.frames / secs, x.late, x.intervalMax, x.waitAvg,
             x.copyAvg, x.copyMax, x.endAvg, p_gameRenders, p_skippedRenders);
    }
    p_frame.reset(); p_pace.reset(); p_stereoCpu.reset();
    p_frames = p_gameRenders = p_skippedRenders = 0;
}

void Report() {
    DWORD now = GetTickCount();
    if (now - s_lastReport < 10000) return;
    s_lastReport = now;
    auto st = xrout::TakeStats();
    LOGI("stats 10s: game frames %u stereo %u menu-only %u no-pose %u | submitted %u no-slot %u | xr frames %u new %u empty %u | %s | world '%s'",
         s_frames, s_stereo, s_menu, s_noPose, st.submitted, st.noSlot, st.xrFrames, st.xrNewFrames, st.xrNoFrame,
         xrout::StateName(), g_worldName.c_str());
    s_frames = s_stereo = s_menu = s_noPose = 0;
}

void ApplyHud() { xrout::SetHud(C.hudDistance, C.hudSize, C.hudOffsetY, C.hudFollowHead); xrout::SetMenuPanel(C.menuDistance, C.menuSize); }

// In-headset settings menu (vrkit). Each option points at the config field it edits; keys are the config.txt keys.
// onChange runs on the XR thread: only plain values / atomics are touched there.
void RegisterMenuOptions() {
    auto& o = vrkit::Options::Get();
    if (!o.All().empty()) return;
    vrkit::SetLog([](const char* m) { LOGI("%s", m); });

    // ---- View
    o.AddBool("View", "head_tracking", "Head tracking", &C.headTracking,
              "Off: the view stays locked to the game camera (only the eye separation is kept).");
    o.AddFloat("View", "world_scale", "World scale", &C.worldScale, 0.1f, 10.f, 0.1f, 1,
               "Game units per metre of head movement. Higher = the world feels smaller.");
    o.AddBool("View", "level_camera", "Level horizon", &C.levelCamera,
              "Remove the game camera's downward tilt (keep its heading) so the horizon stays level.");
    o.AddAction("View", "Recenter view", [] { xrout::Recenter(); }, "Make the current head position the centre (F9).");

    // ---- Display
    std::vector<std::string> names, labels;
    for (auto& p : kPresets) { names.push_back(p.name); labels.push_back(p.label); }
    auto& pr = o.AddChoice("Display", "render_preset", "Resolution", &C.renderPreset, names,
                           "Per-eye render size relative to the headset's recommended size.", [] { ApplyRenderPreset(); });
    pr.choiceLabels = labels;
    o.AddFloat("Display", "render_scale", "Custom scale", &C.renderScale, 0.25f, 2.f, 0.05f, 2,
               "Per-eye scale used by the Custom preset.", [] { ApplyRenderPreset(); })
        .visible = [] { return C.renderPreset == "custom"; };
    o.AddBool("Display", "frame_pacing", "Frame pacing", &C.pacing,
              "Render at most one game frame per headset frame (keeps the GPU from being flooded).");
    o.AddBool("Display", "desktop_world", "3D world on monitor", &C.desktopWorld,
              "Off saves a full-resolution render per frame; the monitor then shows only the game's overlays.");

    // ---- HUD
    o.AddFloat("HUD", "hud_distance", "HUD distance (m)", &C.hudDistance, 0.3f, 10.f, 0.1f, 1, "How far away the HUD panel floats.", ApplyHud);
    o.AddFloat("HUD", "hud_size", "HUD width (m)", &C.hudSize, 0.2f, 10.f, 0.1f, 1, "Width of the HUD panel.", ApplyHud);
    o.AddFloat("HUD", "hud_offset_y", "HUD height (m)", &C.hudOffsetY, -2.f, 2.f, 0.05f, 2, "Up / down offset of the HUD panel.", ApplyHud);
    o.AddBool("HUD", "hud_follow_head", "HUD follows head", &C.hudFollowHead,
              "On: the HUD stays in front of your eyes. Off: it stays where you recentred.", ApplyHud);
    o.AddFloat("HUD", "menu_distance", "Game menu distance (m)", &C.menuDistance, 0.3f, 10.f, 0.1f, 1,
               "Distance of the panel used for the game's full-screen menus.", ApplyHud);
    o.AddFloat("HUD", "menu_size", "Game menu width (m)", &C.menuSize, 0.2f, 10.f, 0.1f, 1,
               "Width of the panel used for the game's full-screen menus.", ApplyHud);
    static bool hudLayerSaved = C.hudInHeadset;   // edited copy: only takes effect after a restart
    o.AddBool("HUD", "hud_layer", "Separate HUD layer", &hudLayerSaved,
              "Show the game's UI on its own panel (off: the UI is not shown in the headset).").needsRestart = true;

    // ---- Debug
    o.AddBool("Debug", "perf_log", "Performance log", &C.perfLog, "One line per second in neovr.log.");
    o.AddAction("Debug", "Save headset images", [] { static uint32_t n = 1000; xrout::RequestDump(++n); },
                "Writes neovr_dump_<n>_left/right/hud.bmp into the game folder (F11).");
}

} // namespace

bool Init(const Config& cfg, const std::wstring& gameDir) {
    C.vr = cfg.GetBool("vr", true);
    if (!C.vr) { LOGI("stereo: vr = 0, VR output off"); return false; }
    C.worldCams = Split(cfg.GetStr("world_cameras", "GameCamera,Camera,Main Camera"));
    C.hudCams = Split(cfg.GetStr("hud_cameras", "UICamera3D,UICamera2D,UICamera2DDialog,UICamera2DBG"));
    C.renderScale = cfg.GetFloat("render_scale", 1.0f);
    C.renderPreset = cfg.GetStr("render_preset", "native");
    ApplyRenderPreset();
    C.eyeW = cfg.GetInt("eye_width", 0);
    C.eyeH = cfg.GetInt("eye_height", 0);
    C.hudW = cfg.GetInt("hud_tex_width", 1920);
    C.hudH = cfg.GetInt("hud_tex_height", 1080);
    C.worldScale = cfg.GetFloat("world_scale", 1.0f);
    C.headTracking = cfg.GetBool("head_tracking", true);
    C.levelCamera = cfg.GetBool("level_camera", false);
    C.hudInHeadset = cfg.GetBool("hud_layer", true);
    C.keyRecenter = cfg.GetInt("key_recenter", VK_F9);
    C.keyHud = cfg.GetInt("key_hud", VK_F10);
    C.keyDump = cfg.GetInt("key_dump", VK_F11);
    C.debugDumpAtSec = cfg.GetFloat("debug_dump_at_sec", 0);
    C.pacing = cfg.GetBool("frame_pacing", true);
    C.pacingTimeoutMs = cfg.GetInt("frame_pacing_timeout_ms", 30);
    C.desktopWorld = cfg.GetBool("desktop_world", true);
    C.perfLog = cfg.GetBool("perf_log", false);
    C.perfSlowMs = cfg.GetFloat("perf_log_slow_ms", 0);

    C.hudDistance = cfg.GetFloat("hud_distance", 1.6f);
    C.hudSize = cfg.GetFloat("hud_size", 1.6f);
    C.hudOffsetY = cfg.GetFloat("hud_offset_y", -0.05f);
    C.hudFollowHead = cfg.GetBool("hud_follow_head", false);
    C.menuDistance = cfg.GetFloat("menu_distance", 2.0f);
    C.menuSize = cfg.GetFloat("menu_size", 2.4f);
    xrout::Settings s;
    s.appName = "NEO TWEWY VR";
    s.dumpDir = gameDir;
    s.hudDistance = C.hudDistance;
    s.hudWidth = C.hudSize;
    s.hudOffsetY = C.hudOffsetY;
    s.hudFollowHead = C.hudFollowHead;
    s.menuDistance = C.menuDistance;
    s.menuWidth = C.menuSize;
    s.flipEyesY = cfg.GetBool("flip_eyes_y", true);
    s.flipHudY = cfg.GetBool("flip_hud_y", true);
    s.flipEyesX = cfg.GetBool("flip_eyes_x", false);
    s.flipHudX = cfg.GetBool("flip_hud_x", false);
    s.settingsMenu = cfg.GetBool("settings_menu", true);
    s.settingsMenuDistance = cfg.GetFloat("settings_menu_distance", 1.0f);
    s.settingsMenuSize = cfg.GetFloat("settings_menu_size", 0.9f);
    s.settingsMenuFollowHead = cfg.GetBool("settings_menu_follow_head", false);
    s.configPath = gameDir + L"\\config.txt";
    s.menuTitle = "NEO: The World Ends with You VR  1.0";
    s.menuStatus = [] {
        char b[96];
        snprintf(b, sizeof(b), "game %.0f fps | headset %.0f fps | eyes %.1f ms GPU", g_statGameFps.load(), g_statXrFps.load(),
                 g_statGpuMs.load());
        return std::string(b);
    };
    RegisterMenuOptions();
    if (!xrout::Init(s)) { LOGE("stereo: OpenXR thread failed to start"); C.vr = false; return false; }
    LOGI("stereo: initialised (world cams %zu, hud cams %zu, scale %.2f)", C.worldCams.size(), C.hudCams.size(), C.renderScale);
    return true;
}

bool InOwnRender() { return g_inOwnRender; }

bool OnGameCameraRender(Object* cam) {
    if (!C.vr || g_inOwnRender || !cam || cam != g_worldCam.get()) return true;
    p_gameRenders++;
    if (C.desktopWorld || !xrout::Ready()) return true;
    // Skipped for the monitor, but it still counts as "the world was rendered this frame" (menu-mode detection).
    if (g_renderedThisFrame.size() < 64) g_renderedThisFrame.push_back(cam);
    p_skippedRenders++;
    return false;
}

void OnCameraPreCull(Object* cam) {
    if (!g_inOwnRender && g_renderedThisFrame.size() < 64) g_renderedThisFrame.push_back(cam);
}

void OnFrame() {
    if (!C.vr) return;
    // Pacing first, so the poses read below are the freshest for the headset frame that just started.
    if (C.pacing && xrout::Ready()) p_pace.add(xrout::WaitForHeadsetFrame(C.pacingTimeoutMs));
    double tf = perf::NowMs();
    if (p_lastFrame) p_frame.add(tf - p_lastFrame);
    p_lastFrame = tf;
    p_frames++;
    g_renderedLastFrame.swap(g_renderedThisFrame);
    g_renderedThisFrame.clear();
    s_frames++;
    Report();
    PerfSecond();

    if (KeyPressed(C.keyRecenter)) { xrout::Recenter(); LOGI("F9: recenter"); }
    if (KeyPressed(C.keyHud)) { g_hudVisible = !g_hudVisible; LOGI("F10: HUD %s", g_hudVisible ? "on" : "off"); }
    bool dumpNow = KeyPressed(C.keyDump);

    if (!xrout::Ready()) return;
    if (!EnsureTargets()) return;

    DWORD now = GetTickCount();
    if (now - g_lastScan > 1000 || (g_worldCam.get() && !Alive(g_worldCam.get()))) {
        g_lastScan = now;
        ScanCameras();
    }
    xrout::FrameInfo fi{};
    if (!xrout::LatestPoses(fi.pose, fi.fov, &fi.time)) { s_noPose++; return; }
    fi.flags = g_hudVisible && C.hudInHeadset ? xrout::FRAME_HUD_VISIBLE : 0;

    Object* cam = g_worldCam.get();
    bool worldRendered = false;
    for (Object* r : g_renderedLastFrame) worldRendered |= (r == cam);
    if (cam && Alive(cam) && worldRendered) {
        Object* tr = Invoke(M.Component_get_transform, cam);
        Vec3 gp{};
        Quat gr{0, 0, 0, 1};
        if (tr) { I.Transform_get_position(tr, &gp); I.Transform_get_rotation(tr, &gr); }
        if (C.levelCamera) gr = YawOnly(gr);
        float n = I.Camera_get_nearClipPlane(cam), f = I.Camera_get_farClipPlane(cam);
        Object* oldTarget = I.Camera_get_targetTexture(cam);
        double ts0 = perf::NowMs();
        perf::SetGameTexture(g_eyeTex[0]);
        I.GL_IssuePluginEvent((void*)&perf::GpuEvent, 1);
        g_inOwnRender = true;
        for (int e = 0; e < 2; e++) RenderEye(cam, e, gp, gr, fi.pose[e], fi.fov[e], n, f);
        g_inOwnRender = false;
        I.GL_IssuePluginEvent((void*)&perf::GpuEvent, 2);
        p_stereoCpu.add(perf::NowMs() - ts0);
        I.Camera_ResetWorldToCameraMatrix(cam);
        I.Camera_ResetProjectionMatrix(cam);
        I.Camera_set_targetTexture(cam, oldTarget);
        s_stereo++;
    } else {
        // No 3D world this frame (full-screen menu, loading): show the UI on the large menu panel.
        fi.flags |= xrout::FRAME_MENU_MODE | xrout::FRAME_HUD_VISIBLE;
        s_menu++;
    }

    if (!g_firstVrTick) g_firstVrTick = now;
    if (dumpNow || (C.debugDumpAtSec > 0 && !g_dumped && now - g_firstVrTick > (DWORD)(C.debugDumpAtSec * 1000))) {
        g_dumped = true;
        xrout::RequestDump(++g_dumpId);
        LOGI("dump %u requested (neovr_dump_%u_*.bmp)", g_dumpId, g_dumpId);
    }

    // Queued in order on the render thread: eyes rendered above -> copy (eyes + last frame's UI) -> clear the HUD for
    // the UI cameras that render later this frame.
    int id = g_ringIdx++ & (kRing - 1);
    g_ring[id] = fi;
    g_ringTex[id] = {{g_eyeTex[0], g_eyeTex[1]}, C.hudInHeadset ? g_hudTex : nullptr};
    I.GL_IssuePluginEvent((void*)&RenderThreadSubmit, id);
    if (C.hudInHeadset) ClearHud();
}

} // namespace uxr::stereo
