#include "unity.h"
#include "../common/log.h"

namespace uxr::unity {

Methods M{};
Icalls I{};

#define ICALL(field, name)                                                        \
    I.field = (decltype(I.field))api.resolve_icall(name);                         \
    if (!I.field) { LOGE("unity: icall %s missing", name); ok = false; }

static bool InitIcalls() {
    bool ok = true;
    ICALL(Camera_Render, "UnityEngine.Camera::Render()")
    ICALL(Camera_set_worldToCameraMatrix, "UnityEngine.Camera::set_worldToCameraMatrix_Injected(UnityEngine.Matrix4x4&)")
    ICALL(Camera_ResetWorldToCameraMatrix, "UnityEngine.Camera::ResetWorldToCameraMatrix()")
    ICALL(Camera_set_projectionMatrix, "UnityEngine.Camera::set_projectionMatrix_Injected(UnityEngine.Matrix4x4&)")
    ICALL(Camera_ResetProjectionMatrix, "UnityEngine.Camera::ResetProjectionMatrix()")
    ICALL(Camera_set_targetTexture, "UnityEngine.Camera::set_targetTexture(UnityEngine.RenderTexture)")
    ICALL(Camera_get_targetTexture, "UnityEngine.Camera::get_targetTexture()")
    ICALL(Camera_get_nearClipPlane, "UnityEngine.Camera::get_nearClipPlane()")
    ICALL(Camera_get_farClipPlane, "UnityEngine.Camera::get_farClipPlane()")
    ICALL(Transform_get_position, "UnityEngine.Transform::get_position_Injected(UnityEngine.Vector3&)")
    ICALL(Transform_get_rotation, "UnityEngine.Transform::get_rotation_Injected(UnityEngine.Quaternion&)")
    ICALL(GL_Clear, "UnityEngine.GL::GLClear_Injected(System.Boolean,System.Boolean,UnityEngine.Color&,System.Single)")
    ICALL(GL_IssuePluginEvent, "UnityEngine.GL::GLIssuePluginEvent(System.IntPtr,System.Int32)")
    ICALL(RenderTexture_GetActive, "UnityEngine.RenderTexture::GetActive()")
    ICALL(RenderTexture_SetActive, "UnityEngine.RenderTexture::SetActive(UnityEngine.RenderTexture)")
    ICALL(RenderTexture_Create, "UnityEngine.RenderTexture::Create()")
    ICALL(RenderTexture_Release, "UnityEngine.RenderTexture::Release()")
    ICALL(Texture_GetNativeTexturePtr, "UnityEngine.Texture::GetNativeTexturePtr()")
    ICALL(Object_set_hideFlags, "UnityEngine.Object::set_hideFlags(UnityEngine.HideFlags)")
    return ok;
}

Object* NewRenderTexture(int w, int h, int depthBits, int format, const char* debugName) {
    Object* rt = api.object_new(M.RenderTexture);
    if (!rt) return nullptr;
    if (M.RenderTexture_ctor4) {
        void* args[] = {&w, &h, &depthBits, &format};
        Exception* ex = nullptr;
        api.runtime_invoke(M.RenderTexture_ctor4, rt, args, &ex);
        if (ex) { LOGE("unity: RenderTexture ctor threw"); return nullptr; }
    } else if (M.RenderTexture_ctor3) {
        void* args[] = {&w, &h, &depthBits};
        Exception* ex = nullptr;
        api.runtime_invoke(M.RenderTexture_ctor3, rt, args, &ex);
        if (ex) { LOGE("unity: RenderTexture ctor threw"); return nullptr; }
    } else { LOGE("unity: no RenderTexture constructor"); return nullptr; }
    I.Object_set_hideFlags(rt, 32 /* DontUnloadUnusedAsset */ | 20 /* DontSave */);
    if (!I.RenderTexture_Create(rt)) LOGW("unity: RenderTexture.Create(%s) returned false", debugName);
    LOGI("unity: created RenderTexture '%s' %dx%d depth %d fmt %d native %p", debugName, w, h, depthBits, format,
         I.Texture_GetNativeTexturePtr(rt));
    return rt;
}

bool Init() {
    const char* core = "UnityEngine.CoreModule.dll";
    M.Object = FindClass("UnityEngine", "Object", core);
    M.Component = FindClass("UnityEngine", "Component", core);
    M.Behaviour = FindClass("UnityEngine", "Behaviour", core);
    M.Camera = FindClass("UnityEngine", "Camera", core);
    M.Transform = FindClass("UnityEngine", "Transform", core);
    M.Texture = FindClass("UnityEngine", "Texture", core);
    M.RenderTexture = FindClass("UnityEngine", "RenderTexture", core);
    M.SystemInfo = FindClass("UnityEngine", "SystemInfo", core);
    M.Screen = FindClass("UnityEngine", "Screen", core);
    M.Time = FindClass("UnityEngine", "Time", core);
    M.Application = FindClass("UnityEngine", "Application", core);
    M.SceneManager = FindClass("UnityEngine.SceneManagement", "SceneManager", core);
    M.GL = FindClass("UnityEngine", "GL", core);
    M.QualitySettings = FindClass("UnityEngine", "QualitySettings", core);
    M.Canvas = FindClass("UnityEngine", "Canvas", "UnityEngine.UIModule.dll");
    if (!M.Object || !M.Camera || !M.Transform) return false;

    M.Object_get_name = FindMethod(M.Object, "get_name", 0);
    M.Object_FindObjectsOfType = FindMethod(M.Object, "FindObjectsOfType", 1);
    M.Component_get_transform = FindMethod(M.Component, "get_transform", 0);
    M.Component_get_gameObject = FindMethod(M.Component, "get_gameObject", 0);
    M.Behaviour_get_enabled = FindMethod(M.Behaviour, "get_enabled", 0);
    M.Behaviour_get_isActiveAndEnabled = FindMethod(M.Behaviour, "get_isActiveAndEnabled", 0);
    M.Camera_get_depth = FindMethod(M.Camera, "get_depth", 0);
    M.Camera_get_cullingMask = FindMethod(M.Camera, "get_cullingMask", 0);
    M.Camera_get_clearFlags = FindMethod(M.Camera, "get_clearFlags", 0);
    M.Camera_get_fieldOfView = FindMethod(M.Camera, "get_fieldOfView", 0);
    M.Camera_get_orthographic = FindMethod(M.Camera, "get_orthographic", 0);
    M.Camera_get_pixelRect = FindMethod(M.Camera, "get_pixelRect", 0);
    M.Camera_get_targetTexture = FindMethod(M.Camera, "get_targetTexture", 0);
    M.Camera_get_nearClipPlane = FindMethod(M.Camera, "get_nearClipPlane", 0);
    M.Camera_get_farClipPlane = FindMethod(M.Camera, "get_farClipPlane", 0);
    M.Camera_FireOnPreCull = FindMethod(M.Camera, "FireOnPreCull", 1);
    M.Camera_FireOnPostRender = FindMethod(M.Camera, "FireOnPostRender", 1);
    M.Transform_get_position = FindMethod(M.Transform, "get_position", 0);
    M.Transform_get_rotation = FindMethod(M.Transform, "get_rotation", 0);
    M.Texture_get_width = FindMethod(M.Texture, "get_width", 0);
    M.Texture_get_height = FindMethod(M.Texture, "get_height", 0);
    if (M.Canvas) {
        M.Canvas_get_renderMode = FindMethod(M.Canvas, "get_renderMode", 0);
        M.Canvas_get_worldCamera = FindMethod(M.Canvas, "get_worldCamera", 0);
        M.Canvas_get_sortingOrder = FindMethod(M.Canvas, "get_sortingOrder", 0);
        M.Canvas_get_isRootCanvas = FindMethod(M.Canvas, "get_isRootCanvas", 0);
        M.Canvas_SendWillRenderCanvases = FindMethod(M.Canvas, "SendWillRenderCanvases", 0);
    }
    if (M.Application) {
        M.Application_InvokeOnBeforeRender = FindMethod(M.Application, "InvokeOnBeforeRender", 0);
    }
    M.SystemInfo_get_graphicsDeviceType = FindMethod(M.SystemInfo, "get_graphicsDeviceType", 0);
    M.Screen_get_width = FindMethod(M.Screen, "get_width", 0);
    M.Screen_get_height = FindMethod(M.Screen, "get_height", 0);
    M.SceneManager_GetActiveScene = FindMethod(M.SceneManager, "GetActiveScene", 0);
    M.RenderTexture_ctor4 = api.class_get_method_from_name(M.RenderTexture, ".ctor", 4);
    M.RenderTexture_ctor3 = api.class_get_method_from_name(M.RenderTexture, ".ctor", 3);
    M.QualitySettings_get_activeColorSpace = FindMethod(M.QualitySettings, "get_activeColorSpace", 0);
    LOGI("unity: RenderTexture ctor(4)=%p ctor(3)=%p", M.RenderTexture_ctor4, M.RenderTexture_ctor3);
    if (!InitIcalls()) return false;
    M.Time_get_frameCount = (int (*)())api.resolve_icall("UnityEngine.Time::get_frameCount()");
    M.Scene_GetNameInternal =
        (String * (*)(int)) api.resolve_icall("UnityEngine.SceneManagement.Scene::GetNameInternal(System.Int32)");
    LOGI("unity: icalls frameCount=%p sceneName=%p", M.Time_get_frameCount, M.Scene_GetNameInternal);
    return true;
}

std::string Name(Object* o) {
    if (!o) return "<null>";
    return ToUtf8((String*)Invoke(M.Object_get_name, o));
}

std::vector<Object*> FindObjectsOfType(Class* k) {
    std::vector<Object*> out;
    void* args[] = {TypeObject(k)};
    Array* a = (Array*)Invoke(M.Object_FindObjectsOfType, nullptr, args);
    if (!a) return out;
    uint32_t n = api.array_length(a);
    Object** d = ArrayData(a);
    out.assign(d, d + n);
    return out;
}

std::string ActiveSceneName() {
    int handle = 0;
    if (!M.Scene_GetNameInternal || !InvokeValue(M.SceneManager_GetActiveScene, nullptr, handle)) return "?";
    return ToUtf8(M.Scene_GetNameInternal(handle));
}

int FrameCount() { return M.Time_get_frameCount ? M.Time_get_frameCount() : -1; }

const char* GraphicsDeviceTypeName(int t) {
    switch (t) {
    case 2: return "Direct3D11";
    case 4: return "Null";
    case 17: return "OpenGLCore";
    case 18: return "Direct3D12";
    case 21: return "Vulkan";
    default: return "other";
    }
}

} // namespace uxr::unity
