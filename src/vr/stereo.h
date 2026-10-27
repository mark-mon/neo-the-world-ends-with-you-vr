// Native per-eye stereo for Unity (built-in pipeline): renders the game's world camera once per eye with the headset
// view/projection, captures the UI cameras into a HUD texture, and hands both to xrout on the render thread.
#pragma once
#include <string>
#include "../common/config.h"
#include "../unity/il2cpp.h"

namespace uxr::stereo {

bool Init(const Config& cfg, const std::wstring& gameDir); // main thread, after unity::Init
void OnFrame();                                              // main thread, once per frame (PostLateUpdate)
void OnCameraPreCull(il2cpp::Object* cam);                   // main thread, every camera render
bool InOwnRender();                                          // true while we are calling Camera.Render ourselves
// The game is about to Render() a camera itself. Returns false to skip it (desktop_world = 0 for the world camera).
bool OnGameCameraRender(il2cpp::Object* cam);

} // namespace uxr::stereo
