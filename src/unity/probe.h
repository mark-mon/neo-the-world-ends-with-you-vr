#pragma once
#include "il2cpp.h"

namespace uxr::probe {
void SetHotkey(int vk);
void OnCameraPreCull(il2cpp::Object* cam); // main thread, every camera render
void Tick();                                // main thread, once per frame
void LogSystem();                           // main thread, once
} // namespace uxr::probe
