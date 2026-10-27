// Native in-process OpenXR output (D3D11). Engine-independent: takes D3D11 textures from the game's device.
//
// Threads:
//   XR thread (ours)   own D3D11 device on the headset's GPU + the OpenXR session; runs the frame loop at the
//                      headset rate (xrWaitFrame), publishes predicted eye poses, and submits the newest game frame
//                      with the poses it was rendered with (the compositor reprojects to the current head pose).
//   game render thread SubmitRenderThread() copies the eyes + HUD into a ring of keyed-mutex shared textures.
//   game main thread   LatestPoses() to build the per-eye cameras.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <string>
#include <functional>

namespace uxr::xrout {

struct Pose { float px, py, pz; float qx, qy, qz, qw; };   // OpenXR convention (RH, +Y up, -Z forward, metres)
struct Fov { float left, right, up, down; };               // radians (left/down negative)

enum FrameFlags : uint32_t {
    FRAME_HUD_VISIBLE = 1,   // show the HUD quad
    FRAME_MENU_MODE = 2,     // no 3D world: show only the HUD image, on the larger menu panel
};

struct Settings {
    std::string appName = "Unity VR";
    std::wstring dumpDir;                 // where debug dumps go
    float hudDistance = 1.6f, hudWidth = 1.6f, hudOffsetY = -0.05f;
    bool hudFollowHead = false;
    float menuDistance = 2.0f, menuWidth = 2.4f;
    bool flipEyesY = true, flipHudY = true;   // Unity D3D render textures are stored upside down
    bool flipEyesX = false, flipHudX = false;
    // In-headset settings menu (vrkit): quad layer, runs on the XR thread with the XR device.
    bool settingsMenu = true;
    float settingsMenuDistance = 1.0f, settingsMenuSize = 0.9f;
    bool settingsMenuFollowHead = false;  // false: fixed in the room where you looked when it opened
    std::wstring configPath;              // the menu saves changed options here when it closes
    std::string menuTitle = "VR mod";
    std::function<std::string()> menuStatus;
};

bool Init(const Settings& s);                 // starts the XR thread (never blocks; retries if no headset)
void Shutdown();

bool Ready();                                 // session running and poses available
bool LatestPoses(Pose pose[2], Fov fov[2], int64_t* time);   // relative to the recentred reference
bool RecommendedSize(unsigned* w, unsigned* h);
void Recenter();
void RequestDump(uint32_t id);                // XR thread writes <dumpDir>\neovr_dump_<id>_{left,right,hud}.bmp
void SetHud(float distance, float width, float offsetY, bool followHead);
void SetMenuPanel(float distance, float width);   // panel for the game's full-screen menus (FRAME_MENU_MODE)
// Allows the settings menu to start (its XInput hooks need MinHook: call after MH_Initialize). Options must already
// be registered with vrkit::Options.
void EnableSettingsMenu();
bool SettingsMenuOpen();

struct FrameInfo {
    Pose pose[2];
    Fov fov[2];
    int64_t time;
    uint32_t flags;    // FrameFlags
};

// Game render thread: copies the eye textures (+ HUD, may be null) into a free shared slot and hands it over.
// viewFormat: the DXGI format the pixels are encoded in (TYPELESS sources are common in Unity).
void SubmitRenderThread(ID3D11Texture2D* left, ID3D11Texture2D* right, ID3D11Texture2D* hud, DXGI_FORMAT viewFormat,
                        const FrameInfo& fi);

// Frame pacing (game main thread): waits until the XR thread has started a new headset frame since the last call, so
// the game renders at most one frame per headset frame. Returns the time waited in ms (0 if a frame was already due).
double WaitForHeadsetFrame(DWORD timeoutMs);

struct Stats { uint32_t submitted, noSlot, recreated, xrFrames, xrNewFrames, xrNoFrame; };
// XR thread timing since the last call (ms): xrWaitFrame blocking time, interval between headset frames, time spent
// copying a new game frame into the swapchains, xrEndFrame time; late = intervals > 1.5 display periods.
struct XrPerf { double waitAvg, intervalAvg, intervalMax, copyAvg, copyMax, endAvg; uint32_t frames, late; double periodMs; };
XrPerf TakeXrPerf();
Stats TakeStats();
const char* StateName();

} // namespace uxr::xrout
