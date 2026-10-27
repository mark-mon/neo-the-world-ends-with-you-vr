# Native VR for Unity games: notes

Written 2026-10-27 after NEO: The World Ends with You VR (`D:\vrconversions\neovr`). It covers what worked, what broke
and how it was fixed, and what to expect from other Unity games.

## 1. The approach: let Unity render the eyes itself

Unity already knows how to render a camera into a texture with any view and projection matrix. So the mod never
replays draw calls or patches constant buffers. Each frame it simply asks Unity to render the game's own camera twice:

```
once per frame, after LateUpdate, before rendering (Application.InvokeOnBeforeRender hook):
    wait for the headset's next frame                      (frame pacing)
    read predicted eye poses + FOVs from the OpenXR thread
    for each eye:
        cam.targetTexture        = eyeRT[eye]              (icall Camera::set_targetTexture)
        cam.worldToCameraMatrix  = view(gameCam pose * head pose * eye offset)
        cam.projectionMatrix     = off-axis projection from the eye FOV
        cam.Render()                                       (icall Camera::Render)
    restore target, ResetWorldToCameraMatrix, ResetProjectionMatrix
    GL.IssuePluginEvent(callback)  -> on Unity's render thread: copy eyes + HUD into a shared texture ring
    clear the HUD render texture (the UI cameras draw into it later this frame)
```

That gives true per-eye stereo: correct culling, shadows and lighting per eye, and no AER. It's "native" in the sense
the getting-started notes ask for.

**HUD.** In NEO every UI canvas was `ScreenSpaceCamera` on dedicated UI cameras (UICamera3D, UICamera2D,
UICamera2DDialog, UICamera2DBG). Setting those cameras' `targetTexture` to a transparent HUD RenderTexture separates
the whole UI with zero game-specific code. If no world camera rendered this frame (a full-screen menu), the frame is
flagged *menu mode* and only the HUD is shown, on a bigger panel.

**OpenXR runs in-process** (`src/vr/xrout.cpp`). There's an XR thread with its own D3D11 device on the headset GPU,
`xrWaitFrame` pacing at the headset rate, a keyed-mutex shared-texture ring between Unity's device and the XR device,
and a projection layer submitted with the poses each frame was rendered with (so the compositor reprojects). The HUD
is a premultiplied quad layer. The OpenXR loader is linked statically, so the mod is a single `version.dll`.
XrBridge is not needed for 64-bit D3D11 games.

## 2. Engine-level access with no offsets

Everything goes through exports and names, so the same DLL works across builds of a game and across games on similar
Unity versions:

- **Loader:** `version.dll` proxy. `UnityPlayer.dll` imports `version.dll` and `winhttp.dll` in every recent Unity, so
  this works for any Unity game (check with pefile: imports of UnityPlayer.dll).
- **IL2CPP runtime:** `GameAssembly.dll` exports `il2cpp_*` (domain, class_from_name, class_get_method_from_name,
  runtime_invoke, resolve_icall, gchandle_*, object_new ...). Wait for `il2cpp_get_corlib() != null` before using it.
- **Managed methods:** `MethodInfo->methodPointer` is the first field, so hooking managed methods with MinHook works
  (static methods: `void f(args..., const MethodInfo*)`; instance: `f(this, args..., const MethodInfo*)`).
- **Internal calls:** `il2cpp_resolve_icall("UnityEngine.Camera::Render()")` gives the native function in
  UnityPlayer.dll directly. Instance icalls take the managed object as their first argument. Use icalls when IL2CPP
  stripping removed the managed wrapper (NEO had no managed `set_worldToCameraMatrix`). The exact names are in
  `strings UnityPlayer.dll | grep "UnityEngine.Camera::"`. Value-type setters are `*_Injected(T&)`.
- **Kept objects:** hold Unity objects across frames with `il2cpp_gchandle_new` (strong). Detect destroyed objects by
  `m_CachedPtr` (the first field after the 16-byte object header) becoming null.

## 3. Pitfalls we hit (all fixed in core/)

| Symptom | Cause | Fix |
|---|---|---|
| Headset black, "submitted 0" | `UnityEngine.GL::IssuePluginEvent` is the **legacy (int eventID)** icall | Use `UnityEngine.GL::GLIssuePluginEvent(System.IntPtr,System.Int32)` |
| Whole image upside down (looked like a 180° rotation) | Unity stores D3D render textures **vertically flipped** | XR thread copies with a tiny flip shader (`flip_eyes_y`, `flip_hud_y`); bit-exact via UNORM/SRGB view pairs on a TYPELESS atlas |
| World camera flickering between found and "none", 2/3 of frames in menu mode | `Canvas.SendWillRenderCanvases` fires **several times per frame** (ForceUpdateCanvases) | Run the per-frame work from `Application.InvokeOnBeforeRender`, deduplicated by `Time.frameCount` |
| 90 → 15 fps in battles | Game runs uncapped (400+ fps); the GPU queue floods and starves the compositor | Frame pacing: the main thread waits for the next `xrWaitFrame` before each frame (`frame_pacing = 1`) |
| No VR at all (first try) | Asked for the recommended size with a 0 ms timeout and never polled | Size is known as soon as the session exists |
| A manually rendered world camera reads as "disabled" | Games often keep their main camera disabled and call `Camera.Render()` themselves | Pick the world camera by *what rendered last frame* (FireOnPreCull list), not by `enabled` |

Other details worth knowing:
- Unity camera space is right-handed with -Z forward: `worldToCameraMatrix = scale(1,1,-1) * inverse(TRS)`. To go from
  an OpenXR pose to Unity, mirror Z: pos `(x, y, -z)`, quat `(-x, -y, z, w)`. Projection: standard OpenGL off-axis;
  Unity converts it for D3D itself.
- ARGB32 RenderTextures are `R8G8B8A8_TYPELESS` on D3D11. In Gamma colour space the bits are sRGB-encoded, so copy them
  bit-exact into an `_SRGB` swapchain. In Linear space Unity uses sRGB RTs, so the same rule applies.
- UI drawn over transparent black gives premultiplied colour. Submit the HUD quad with
  `BLEND_TEXTURE_SOURCE_ALPHA` (premultiplied). Alpha of semi-transparent UI is slightly low (alpha²). Use the
  black/white matte trick if it matters.
- Our own `Camera.Render` calls also fire `FireOnPreCull`. Keep an "in own render" flag so they don't pollute the
  per-frame camera list.
- Hooking the managed `Camera.Render` lets you skip the game's desktop render of the world camera (`desktop_world = 0`).

## 4. Will other Unity games be this easy?

Many will, but not all. NEO was close to the best case: built-in pipeline, D3D11, every UI canvas on its own
camera, one world camera, and a fixed third-person camera with no mouse look. Expect:

**Easy (NEO path almost unchanged):** Unity 2017–2022, IL2CPP x64, built-in render pipeline, D3D11, third-person or
fixed cameras. This covers a lot of Japanese console ports (Square Enix, Falcom-style, NIS) and many indies.

**Moderate, extra work in the engine layer:**
- **Mono builds** (`<game>_Data\Managed\Assembly-CSharp.dll`, `mono-2.0-bdwgc.dll`, no GameAssembly.dll): same idea,
  but through the `mono_*` exports (`mono_class_from_name`, `mono_runtime_invoke`, `mono_compile_method` for hooks,
  `mono_add_internal_call` / icall lookup differs). Write a `mono.cpp` backend behind the same `unity.*` interface. A
  bonus is that decompiling Assembly-CSharp with ILSpy shows the game logic directly.
- **Screen Space Overlay canvases** (no UI camera): set `canvas.renderMode = ScreenSpaceCamera` and
  `worldCamera = <our HUD camera>` (a camera we create that renders only the UI layer into the HUD RT).
- **Post-processing Stack v2 / TAA:** temporal effects keep history per camera, so rendering one camera twice per frame
  ghosts or blurs. Use two cloned cameras (one per eye, each with its own history), or turn TAA off and use SMAA/FXAA.
- **Image effects using `Screen.width/height`** or fixed 16:9 assumptions: render the eyes at 16:9 crops, or patch
  the effect.
- **First-person / mouse-look games:** head tracking must be merged into the game's yaw (otherwise you get double
  rotation). Usually it's enough to hook the camera controller and remove pitch.
- **D3D12 / Vulkan:** the Unity side is identical; only the shared-texture hand-off in `xrout.cpp` changes (D3D12 shared
  heaps/fences, or force `-force-d3d11` on the command line, which most Unity games accept).

**Hard:**
- **URP / HDRP (Scriptable Render Pipelines):** `Camera.Render()` still works in URP 2020+, but HDRP has
  per-camera history, volumetrics and exposure that dislike two renders per frame. Better route: Unity's own XR
  (`XR Plugin Management` + OpenXR) can't be enabled after the fact in IL2CPP builds, so either render two camera
  clones or hook `RenderPipelineManager`. AvalonVR (HDRP) is the reference.
- **Obfuscated IL2CPP** (BeeByte/renamed classes): runtime lookups of game classes by name fail, but UnityEngine
  classes keep their names, so the engine layer still works. Encrypted `global-metadata.dat` doesn't matter at runtime.
- **Anti-cheat** (EAC/BattlEye): online games, don't.

## 5. What NEO didn't need yet (todo for the kit)
- vrkit in-headset menu (`sharedtools\vrkit`): it plugs into `xrout` as one more quad layer.
- Render-scale presets, NVIDIA VRS foveation (`vr-mod-toolkit.md`), eye gaze.
- Matte HUD alpha, desktop mirror of the left eye when `desktop_world = 0`.
- A Mono backend.
