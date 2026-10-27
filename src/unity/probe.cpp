// Diagnostics: logs which cameras/canvases exist and which cameras actually render each frame.
// Runs on Unity's main thread from the frame tick.
#include "probe.h"
#include "unity.h"
#include "../common/log.h"
#include <algorithm>
#include <set>

namespace uxr::probe {
using namespace uxr::unity;

static std::vector<Object*> g_frameRenders;   // cameras seen in FireOnPreCull since the last tick
static std::vector<Object*> g_lastRenderList;
static std::string g_lastScene;
static std::string g_lastCamSig;
static int g_hotkey = VK_F8;
static bool g_keyDown = false;
static DWORD g_lastScan = 0;

void SetHotkey(int vk) { g_hotkey = vk; }

void OnCameraPreCull(Object* cam) {
    if (g_frameRenders.size() < 256) g_frameRenders.push_back(cam);
}

static std::string CamLine(Object* cam) {
    char buf[1024];
    bool en = false, act = false, ortho = false;
    float depth = 0, fov = 0, nearc = 0, farc = 0;
    int mask = 0, clear = 0;
    Rect r{};
    Vec3 p{};
    Quat q{};
    InvokeValue(M.Behaviour_get_enabled, cam, en);
    InvokeValue(M.Behaviour_get_isActiveAndEnabled, cam, act);
    InvokeValue(M.Camera_get_depth, cam, depth);
    InvokeValue(M.Camera_get_cullingMask, cam, mask);
    InvokeValue(M.Camera_get_clearFlags, cam, clear);
    InvokeValue(M.Camera_get_fieldOfView, cam, fov);
    InvokeValue(M.Camera_get_orthographic, cam, ortho);
    InvokeValue(M.Camera_get_pixelRect, cam, r);
    InvokeValue(M.Camera_get_nearClipPlane, cam, nearc);
    InvokeValue(M.Camera_get_farClipPlane, cam, farc);
    Object* tr = Invoke(M.Component_get_transform, cam);
    if (tr) { InvokeValue(M.Transform_get_position, tr, p); InvokeValue(M.Transform_get_rotation, tr, q); }
    Object* rt = Invoke(M.Camera_get_targetTexture, cam);
    char rtDesc[160] = "screen";
    if (rt) {
        int w = 0, h = 0;
        InvokeValue(M.Texture_get_width, rt, w);
        InvokeValue(M.Texture_get_height, rt, h);
        snprintf(rtDesc, sizeof(rtDesc), "RT '%s' %dx%d", Name(rt).c_str(), w, h);
    }
    snprintf(buf, sizeof(buf),
             "'%s' %s depth=%.1f mask=0x%08x clear=%d %s=%.2f clip=%.2f..%.1f rect=(%.0f,%.0f %.0fx%.0f) -> %s "
             "pos=(%.2f,%.2f,%.2f) rot=(%.3f,%.3f,%.3f,%.3f) [%p]",
             Name(cam).c_str(), act ? "ACTIVE" : (en ? "enabled(inactive go)" : "disabled"), depth, mask, clear,
             ortho ? "orthoSize" : "fov", fov, nearc, farc, r.x, r.y, r.w, r.h, rtDesc, p.x, p.y, p.z, q.x, q.y, q.z,
             q.w, cam);
    return buf;
}

static void DumpAll(const char* why) {
    LOGI("==== probe dump (%s) frame=%d scene='%s' ====", why, FrameCount(), ActiveSceneName().c_str());
    auto cams = FindObjectsOfType(M.Camera);
    LOGI("cameras: %zu", cams.size());
    for (auto* c : cams) LOGI("  cam %s", CamLine(c).c_str());
    if (M.Canvas) {
        auto cvs = FindObjectsOfType(M.Canvas);
        int roots = 0;
        for (auto* c : cvs) {
            bool root = false;
            InvokeValue(M.Canvas_get_isRootCanvas, c, root);
            if (!root) continue;
            roots++;
            int mode = 0, order = 0;
            bool act = false;
            InvokeValue(M.Canvas_get_renderMode, c, mode);
            InvokeValue(M.Canvas_get_sortingOrder, c, order);
            InvokeValue(M.Behaviour_get_isActiveAndEnabled, c, act);
            Object* wc = Invoke(M.Canvas_get_worldCamera, c);
            static const char* modes[] = {"Overlay", "ScreenSpaceCamera", "WorldSpace"};
            LOGI("  canvas '%s' %s mode=%s order=%d camera='%s'", Name(c).c_str(), act ? "ACTIVE" : "inactive",
                 mode >= 0 && mode < 3 ? modes[mode] : "?", order, wc ? Name(wc).c_str() : "-");
        }
        LOGI("canvases: %zu total, %d root", cvs.size(), roots);
    }
    std::string order;
    for (auto* c : g_lastRenderList) order += "'" + Name(c) + "' ";
    LOGI("render order last frame (%zu): %s", g_lastRenderList.size(), order.c_str());
    LogFlush();
}

void Tick() {
    // Frame boundary: everything collected since the last tick is the previous frame's render list.
    if (g_frameRenders != g_lastRenderList) {
        std::string order;
        for (auto* c : g_frameRenders) order += "'" + Name(c) + "' ";
        LOGI("render list changed (frame %d, %zu cams): %s", FrameCount(), g_frameRenders.size(), order.c_str());
        g_lastRenderList = g_frameRenders;
    }
    g_frameRenders.clear();

    bool down = (GetAsyncKeyState(g_hotkey) & 0x8000) != 0;
    if (down && !g_keyDown) DumpAll("hotkey");
    g_keyDown = down;

    DWORD now = GetTickCount();
    if (now - g_lastScan > 1000) {
        g_lastScan = now;
        std::string scene = ActiveSceneName();
        auto cams = FindObjectsOfType(M.Camera);
        std::string sig;
        for (auto* c : cams) { char b[32]; snprintf(b, sizeof(b), "%p,", c); sig += b; }
        if (scene != g_lastScene) {
            LOGI("scene changed: '%s' -> '%s'", g_lastScene.c_str(), scene.c_str());
            g_lastScene = scene;
            DumpAll("scene change");
            g_lastCamSig = sig;
        } else if (sig != g_lastCamSig) {
            g_lastCamSig = sig;
            DumpAll("camera set changed");
        }
    }
}

void LogSystem() {
    int dev = -1, w = 0, h = 0;
    InvokeValue(M.SystemInfo_get_graphicsDeviceType, nullptr, dev);
    InvokeValue(M.Screen_get_width, nullptr, w);
    InvokeValue(M.Screen_get_height, nullptr, h);
    LOGI("graphics device type = %d (%s), screen %dx%d", dev, GraphicsDeviceTypeName(dev), w, h);
    const char* mods[] = {"d3d11.dll", "d3d12.dll", "dxgi.dll", "vulkan-1.dll", "opengl32.dll", "openxr_loader.dll",
                          "openvr_api.dll", "GameOverlayRenderer64.dll", "DxInputControllerPlugin.dll"};
    for (auto* m : mods) LOGI("module %-28s %s", m, GetModuleHandleA(m) ? "loaded" : "-");
}

} // namespace uxr::probe
