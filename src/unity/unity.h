// Cached UnityEngine classes/methods (engine level, Unity 2019.x built-in pipeline, IL2CPP).
#pragma once
#include "il2cpp.h"
#include <string>
#include <vector>

namespace uxr::unity {
using namespace uxr::il2cpp;

struct Methods {
    Class *Object, *Component, *GL, *QualitySettings, *Behaviour, *Camera, *Transform, *Texture, *RenderTexture, *Canvas, *SystemInfo,
        *Screen, *SceneManager, *Time, *Application;
    const MethodInfo *Object_get_name, *Object_FindObjectsOfType;
    const MethodInfo *Component_get_transform, *Component_get_gameObject;
    const MethodInfo *Behaviour_get_enabled, *Behaviour_get_isActiveAndEnabled;
    const MethodInfo *Camera_get_depth, *Camera_get_cullingMask, *Camera_get_clearFlags, *Camera_get_fieldOfView,
        *Camera_get_orthographic, *Camera_get_pixelRect, *Camera_get_targetTexture, *Camera_FireOnPreCull,
        *Camera_FireOnPostRender, *Camera_get_nearClipPlane, *Camera_get_farClipPlane;
    const MethodInfo *Transform_get_position, *Transform_get_rotation;
    const MethodInfo *Texture_get_width, *Texture_get_height;
    const MethodInfo *Canvas_get_renderMode, *Canvas_get_worldCamera, *Canvas_get_sortingOrder, *Canvas_get_isRootCanvas,
        *Canvas_SendWillRenderCanvases;
    const MethodInfo *SystemInfo_get_graphicsDeviceType, *Screen_get_width, *Screen_get_height;
    const MethodInfo *SceneManager_GetActiveScene, *Application_InvokeOnBeforeRender;
    const MethodInfo *RenderTexture_ctor4, *RenderTexture_ctor3, *QualitySettings_get_activeColorSpace;
    int (*Time_get_frameCount)();               // icall
    String* (*Scene_GetNameInternal)(int);      // icall
};
extern Methods M;

// Direct internal calls (UnityPlayer.dll). Instance icalls take the managed object as their first argument.
struct Icalls {
    void (*Camera_Render)(Object* cam);
    void (*Camera_set_worldToCameraMatrix)(Object* cam, const Matrix4x4* m);
    void (*Camera_ResetWorldToCameraMatrix)(Object* cam);
    void (*Camera_set_projectionMatrix)(Object* cam, const Matrix4x4* m);
    void (*Camera_ResetProjectionMatrix)(Object* cam);
    void (*Camera_set_targetTexture)(Object* cam, Object* rt);
    Object* (*Camera_get_targetTexture)(Object* cam);
    float (*Camera_get_nearClipPlane)(Object* cam);
    float (*Camera_get_farClipPlane)(Object* cam);
    void (*Transform_get_position)(Object* tr, Vec3* out);
    void (*Transform_get_rotation)(Object* tr, Quat* out);
    void (*GL_Clear)(bool clearDepth, bool clearColor, const Color* c, float depth);
    void (*GL_IssuePluginEvent)(void* callback, int eventId);
    Object* (*RenderTexture_GetActive)();
    void (*RenderTexture_SetActive)(Object* rt);
    bool (*RenderTexture_Create)(Object* rt);
    void (*RenderTexture_Release)(Object* rt);   // frees the GPU texture
    void* (*Texture_GetNativeTexturePtr)(Object* tex);
    void (*Object_set_hideFlags)(Object* o, int flags);
};
extern Icalls I;

// Creates a RenderTexture (ARGB32 unless format given), kept alive by the returned handle. Main thread.
Object* NewRenderTexture(int w, int h, int depthBits, int format, const char* debugName);

bool Init(); // resolve everything; call after il2cpp runtime is up

std::string Name(Object* o);
std::vector<Object*> FindObjectsOfType(Class* k);
std::string ActiveSceneName();
int FrameCount();
const char* GraphicsDeviceTypeName(int t);

} // namespace uxr::unity
