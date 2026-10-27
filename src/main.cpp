// NEO TWEWY VR - entry point. Loaded by UnityPlayer.dll as version.dll (see loader/version_proxy.cpp).
// Stage 0: diagnostics probe (no VR yet). Waits for the IL2CPP runtime, hooks Unity's per-camera and per-frame
// callbacks on the main thread, and logs the graphics API, cameras, canvases and per-frame render order.
#include <windows.h>
#include <string>
#include "common/log.h"
#include "common/config.h"
#include "unity/il2cpp.h"
#include "unity/unity.h"
#include "unity/probe.h"
#include "vr/stereo.h"
#include "vr/xrout.h"
#include "MinHook.h"

#define NEOVR_VERSION "1.0.0"

using namespace uxr;

static std::wstring g_gameDir;
static Config g_cfg;
static bool g_probe = true;
static bool g_vr = false;

static const char kDefaultConfig[] =
    "# NEO: The World Ends with You VR - configuration\n"
    "# key = value. Lines starting with # are comments. Restart the game after editing.\n"
    "\n"
    "# 0 = mod does nothing at all\n"
    "enabled = 1\n"
    "# 0 error, 1 warn, 2 info, 3 debug, 4 trace\n"
    "log_level = 2\n"
    "\n"
    "# ---- VR output (native OpenXR, SteamVR or any OpenXR runtime)\n"
    "vr = 1\n"
    "# Per-eye render size: headset recommended size x preset (performance 70%, balanced 85%, native 100%, high 120%,\n"
    "# ultra 140%, custom = render_scale), or eye_width/eye_height if both > 0\n"
    "render_preset = native\n"
    "render_scale = 1.0\n"
    "eye_width = 0\n"
    "eye_height = 0\n"
    "# Head tracking moves the game camera with your head. world_scale = game units per metre of head motion\n"
    "head_tracking = 1\n"
    "world_scale = 1.0\n"
    "# 1 = remove the game camera's downward tilt (keep its heading) so the horizon stays level\n"
    "level_camera = 0\n"
    "\n"
    "# Unity renders into D3D textures upside down; the headset images are flipped back (set 0 if upside down)\n"
    "flip_eyes_y = 1\n"
    "flip_hud_y = 1\n"
    "# Mirror left-right (only if the image looks mirrored)\n"
    "flip_eyes_x = 0\n"
    "flip_hud_x = 0\n"
    "\n"
    "# ---- Performance\n"
    "# Render at most one game frame per headset frame (stops the game from flooding the GPU)\n"
    "frame_pacing = 1\n"
    "# 0 = do not also render the 3D world to the monitor (saves a full-resolution render per frame; the monitor then\n"
    "#     shows only what the game draws on top)\n"
    "desktop_world = 1\n"
    "# One performance line per second in neovr.log (perf_log_slow_ms > 0: only seconds with a slower game frame)\n"
    "perf_log = 0\n"
    "perf_log_slow_ms = 0\n"
    "\n"
    "# ---- HUD / UI on its own panel\n"
    "hud_layer = 1\n"
    "hud_tex_width = 1920\n"
    "hud_tex_height = 1080\n"
    "hud_distance = 1.6\n"
    "hud_size = 1.6\n"
    "hud_offset_y = -0.05\n"
    "hud_follow_head = 0\n"
    "# Full-screen menus (no 3D world) are shown on a larger panel\n"
    "menu_distance = 2.0\n"
    "menu_size = 2.4\n"
    "\n"
    "# ---- In-headset settings menu: hold L3+R3 (both stick clicks) or press Insert. Changes are saved here on close.\n"
    "settings_menu = 1\n"
    "settings_menu_distance = 1.0\n"
    "settings_menu_size = 0.9\n"
    "# 0 = the menu stays fixed in the room where you looked when it opened; 1 = it follows your head\n"
    "settings_menu_follow_head = 0\n"
    "\n"
    "# ---- Cameras (names as logged in neovr.log). The first world camera that renders is used.\n"
    "world_cameras = GameCamera,Camera,Main Camera\n"
    "hud_cameras = UICamera3D,UICamera2D,UICamera2DDialog,UICamera2DBG\n"
    "\n"
    "# ---- Keys (virtual-key codes): F9 recenter, F10 HUD on/off, F11 debug dump of the headset images\n"
    "key_recenter = 0x78\n"
    "key_hud = 0x79\n"
    "key_dump = 0x7A\n"
    "\n"
    "# ---- Diagnostics\n"
    "# Logs every camera / canvas on scene changes and on the hotkey (F8) - for adapting the mod\n"
    "probe = 0\n"
    "probe_hotkey = 0x77\n"
    "# Testing: request a headset image dump this many seconds after VR starts (0 = off)\n"
    "debug_dump_at_sec = 0\n";

// ---- hooks (Unity main thread)
typedef void (*FireOnPreCull_t)(il2cpp::Object* cam, const il2cpp::MethodInfo* mi);
typedef void (*SendWillRenderCanvases_t)(const il2cpp::MethodInfo* mi);
static FireOnPreCull_t o_FireOnPreCull = nullptr;
static SendWillRenderCanvases_t o_SendWillRenderCanvases = nullptr;
static SendWillRenderCanvases_t o_InvokeOnBeforeRender = nullptr;   // same signature: static void (MethodInfo*)
static int g_lastStereoFrame = -2;
static int g_beforeRenderFrames = 0;   // how often the preferred per-frame hook has fired

// Stereo runs once per frame after LateUpdate and before rendering. Preferred: Application.InvokeOnBeforeRender
// (exactly once per frame). Fallback: the first Canvas.SendWillRenderCanvases of a frame (the game can force extra
// canvas updates, so it may fire several times per frame).
static void StereoTick(bool fromBeforeRender) {
    if (!g_vr) return;
    if (fromBeforeRender) g_beforeRenderFrames++;
    else if (g_beforeRenderFrames > 0) return;
    int f = unity::FrameCount();
    if (f == g_lastStereoFrame) return;
    g_lastStereoFrame = f;
    stereo::OnFrame();
}
static int g_lastTickFrame = -2;
static bool g_firstTick = true;

static void FrameTick() {
    int f = unity::FrameCount();
    if (f == g_lastTickFrame) return;
    g_lastTickFrame = f;
    if (g_firstTick) {
        g_firstTick = false;
        LOGI("first main-thread tick on thread %lu (frame %d)", GetCurrentThreadId(), f);
        probe::LogSystem();
    }
    if (g_probe) probe::Tick();
}

static void h_SendWillRenderCanvases(const il2cpp::MethodInfo* mi) {
    FrameTick();
    StereoTick(false);
    o_SendWillRenderCanvases(mi);
}

typedef void (*CameraRender_t)(il2cpp::Object* cam, const il2cpp::MethodInfo* mi);
static CameraRender_t o_CameraRender = nullptr;
// Camera.Render() called by game scripts (our own eye renders call the icall directly and never come through here).
static void h_CameraRender(il2cpp::Object* cam, const il2cpp::MethodInfo* mi) {
    if (g_vr && !stereo::OnGameCameraRender(cam)) return;
    o_CameraRender(cam, mi);
}

static void h_InvokeOnBeforeRender(const il2cpp::MethodInfo* mi) {
    StereoTick(true);
    o_InvokeOnBeforeRender(mi);
}

static void h_FireOnPreCull(il2cpp::Object* cam, const il2cpp::MethodInfo* mi) {
    FrameTick(); // fallback when no canvas exists
    if (g_vr) stereo::OnCameraPreCull(cam);
    if (g_probe && !stereo::InOwnRender()) probe::OnCameraPreCull(cam);
    o_FireOnPreCull(cam, mi);
}

static bool HookMethod(const il2cpp::MethodInfo* m, void* detour, void** orig, const char* what) {
    void* target = il2cpp::MethodPointer(m);
    if (!target) { LOGE("hook %s: no method pointer", what); return false; }
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        LOGE("hook %s: MinHook failed", what);
        return false;
    }
    LOGI("hooked %s at GameAssembly+0x%llx", what, (unsigned long long)((char*)target - (char*)il2cpp::gameAssembly));
    return true;
}

static DWORD WINAPI InitThread(void*) {
    HMODULE ga = nullptr;
    for (int i = 0; i < 120000 / 5 && !(ga = GetModuleHandleW(L"GameAssembly.dll")); i++) Sleep(5);
    if (!ga) { LOGE("GameAssembly.dll never loaded (not an IL2CPP game?)"); return 0; }
    LOGI("GameAssembly.dll at %p", ga);
    if (!il2cpp::LoadApi(ga)) { LOGE("il2cpp API incomplete; giving up"); return 0; }
    if (!il2cpp::WaitForRuntime(120000)) { LOGE("il2cpp runtime did not initialise"); return 0; }
    Sleep(200);
    il2cpp::AttachThisThread();
    LOGI("il2cpp runtime up");
    if (!unity::Init()) { LOGE("UnityEngine classes not found"); return 0; }
    g_vr = stereo::Init(g_cfg, g_gameDir);

    if (MH_Initialize() != MH_OK) { LOGE("MH_Initialize failed"); return 0; }
    if (g_vr) xrout::EnableSettingsMenu();
    HookMethod(unity::M.Camera_FireOnPreCull, (void*)h_FireOnPreCull, (void**)&o_FireOnPreCull, "Camera.FireOnPreCull");
    if (unity::M.Canvas_SendWillRenderCanvases)
        HookMethod(unity::M.Canvas_SendWillRenderCanvases, (void*)h_SendWillRenderCanvases,
                   (void**)&o_SendWillRenderCanvases, "Canvas.SendWillRenderCanvases");
    if (const il2cpp::MethodInfo* render = il2cpp::FindMethod(unity::M.Camera, "Render", 0))
        HookMethod(render, (void*)h_CameraRender, (void**)&o_CameraRender, "Camera.Render (managed)");
    if (unity::M.Application_InvokeOnBeforeRender)
        HookMethod(unity::M.Application_InvokeOnBeforeRender, (void*)h_InvokeOnBeforeRender,
                   (void**)&o_InvokeOnBeforeRender, "Application.InvokeOnBeforeRender");
    LogFlush();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_gameDir = exe;
    g_gameDir = g_gameDir.substr(0, g_gameDir.find_last_of(L"\\/"));
    // Only activate inside the game itself (UnityCrashHandler etc. may also load version.dll from here).
    std::wstring exeName = std::wstring(exe).substr(g_gameDir.size() + 1);
    if (_wcsicmp(exeName.c_str(), L"UnityCrashHandler64.exe") == 0) return TRUE;

    std::wstring cfgPath = g_gameDir + L"\\config.txt";
    WriteDefaultConfig(cfgPath.c_str(), kDefaultConfig);
    g_cfg.Load(cfgPath.c_str());
    if (!g_cfg.GetBool("enabled", true)) return TRUE;

    LogInit((g_gameDir + L"\\neovr.log").c_str(), g_cfg.GetInt("log_level", LOG_INFO));
    LOGI("NEO TWEWY VR " NEOVR_VERSION " built " __DATE__ " " __TIME__ "; exe '%ls'", exeName.c_str());
    g_cfg.Dump();
    g_probe = g_cfg.GetBool("probe", false);
    probe::SetHotkey(g_cfg.GetInt("probe_hotkey", VK_F8));

    CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
    return TRUE;
}
