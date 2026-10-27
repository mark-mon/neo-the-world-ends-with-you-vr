# NEO: The World Ends with You — findings

Game: Unity 2019.4.24f1, IL2CPP x64 (metadata v24, plain), Direct3D 11, built-in render pipeline. Scenes: Boot, Logo,
Title, Field, Battle. `UnityPlayer.dll` imports `version.dll` and `winhttp.dll`, so `version.dll` is the proxy.

## Cameras (from the probe)
| Camera | Role | Notes |
|---|---|---|
| GameCamera | 3D world in Field and Battle | Its component reads as *disabled*: the game renders it itself. Clear = Skybox, mask 0x7ffef9df, FOV 60, clip 0.3–1000 |
| Camera / Main Camera | 3D world on Title / Logo / Boot | |
| UICamera3D, UICamera2D, UICamera2DDialog, UICamera2DBG | all UI | Every canvas is ScreenSpaceCamera on these cameras → redirected into the HUD texture |
| EffectCamera, ComicEventManager, WallGraffitiManager, Mask, DrawMesh_* | render-to-texture helpers | left alone |

When no world camera renders (full-screen menus, UICamera2DBG backgrounds), the frame is shown in menu mode: HUD only,
on the larger panel.

## Things that mattered
- The game runs uncapped (400+ fps in the field). Without frame pacing, battle effects saturated the GPU and the
  headset dropped from 90 to ~15–30 fps. Pacing to `xrWaitFrame` fixed it.
- `Canvas.SendWillRenderCanvases` fires several times per frame here; the per-frame work runs from
  `Application.InvokeOnBeforeRender`.
- `GL::IssuePluginEvent` is the legacy int-only icall; the callback version is `GL::GLIssuePluginEvent`.
- Unity's D3D render textures are vertically flipped; the OpenXR thread flips them with a small shader when copying.
