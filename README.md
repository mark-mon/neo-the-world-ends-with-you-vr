# NEO: The World Ends with You — VR mod

A native VR mod for the Steam version of **NEO: The World Ends with You**. You play in third person with a gamepad, as
designed, but the game is rendered in true stereo 3D in your headset through OpenXR (SteamVR or any OpenXR runtime). The
HUD and menus float on their own panel.

- **True per-eye stereo:** the game's own camera renders each eye with the headset's view, so you get real depth with
  no alternate-eye rendering and no flat screen.
- **Head tracking:** look around the scene from the game's camera position.
- **Separate HUD / UI layer:** the game's interface is shown on a panel in front of you instead of being painted into
  the 3D view. Full-screen menus switch to a larger panel automatically.
- **In-headset settings menu:** hold **L3 + R3** (click both sticks) or press **Insert**.
- **Resolution presets** from 70% to 140% of your headset's recommended resolution.
- **Frame pacing** to the headset's refresh rate, so battles stay smooth.
- **Single file:** one `version.dll`. No other files to install, no injector.

## Requirements
- NEO: The World Ends with You (Steam, Windows, the current version).
- A PC VR headset with an OpenXR runtime. Tested with SteamVR; other OpenXR runtimes (Oculus/Meta, WMR, Virtual
  Desktop, Pimax) should work.
- A gamepad (XInput / Xbox layout recommended).
- A GPU able to render the game twice at your headset's resolution. Developed on an RTX 5090 at 3980×3424 per eye with
  a large margin. Lower the resolution preset on slower GPUs.

## Install
1. Download `NEO_TWEWY_VR_v1.0.zip` from the [Releases](../../releases) page.
2. Copy `version.dll` into the game folder, next to `NEO The World Ends with You.exe`. In Steam: right-click the game →
   Manage → Browse local files.
3. Start your VR runtime (for example SteamVR), then start the game from Steam as usual.

The first start creates `config.txt` (all settings, with comments) and `neovr.log` in the game folder.

**Uninstall:** delete `version.dll` (and optionally `config.txt` and `neovr.log`).

## Controls
| Action | Gamepad | Keyboard |
|---|---|---|
| Open / close the VR settings menu | hold L3 + R3 | Insert |
| Menu: select / change / toggle | D-pad or left stick, A | arrow keys, Enter |
| Menu: reset a value / switch page / close | Y / LB, RB / B | Delete / PgUp, PgDn / Backspace |
| Recenter view | menu → View → Recenter | F9 |
| Show / hide the HUD panel | | F10 |

Settings changed in the menu apply immediately and are saved to `config.txt` when the menu closes. The menu opens in
front of where you're looking and stays fixed in the room (`settings_menu_follow_head = 1` makes it follow your
head instead).

## Settings
Everything is in `config.txt` in the game folder. The full annotated default is in
[docs/config.example.txt](docs/config.example.txt). The most useful settings:

| Key | Default | Meaning |
|---|---|---|
| `render_preset` | `native` | `performance` 70%, `balanced` 85%, `native` 100%, `high` 120%, `ultra` 140%, `custom` (= `render_scale`) of the headset's recommended per-eye size |
| `head_tracking` | `1` | `0` keeps the view locked to the game camera |
| `world_scale` | `1.0` | Game units per metre of head movement (higher = the world feels smaller) |
| `level_camera` | `0` | `1` removes the game camera's downward tilt so the horizon stays level |
| `hud_distance`, `hud_size`, `hud_offset_y` | `1.6`, `1.6`, `-0.05` | HUD panel placement in metres |
| `hud_follow_head` | `0` | `1` keeps the HUD in front of your eyes |
| `menu_distance`, `menu_size` | `2.0`, `2.4` | Panel used for the game's full-screen menus |
| `frame_pacing` | `1` | Renders at most one game frame per headset frame. Keep it on. |
| `desktop_world` | `1` | `0` skips drawing the 3D world on the monitor (saves GPU time) |

## Troubleshooting
- **The game starts on the monitor only:** make sure the VR runtime is running and is the active OpenXR runtime. In
  SteamVR: Settings → OpenXR → "Set SteamVR as OpenXR runtime". Check `neovr.log` for lines starting with `xr:`.
- **Low frame rate:** choose a lower resolution preset in the menu (Display → Resolution), or set `desktop_world = 0`.
  `perf_log = 1` writes one performance line per second to `neovr.log`.
- **The menu reacts but the game also moves:** the mod blocks XInput gamepads while its menu is open. Other input APIs
  are not blocked.
- **The image is upside down or mirrored:** see the `flip_*` keys in `config.txt`. This shouldn't be needed for this
  game.
- **Reporting a problem:** please attach `neovr.log` and `config.txt`.

## How it works
The mod is a `version.dll` proxy that Unity loads at startup. It talks to the game's Unity runtime (IL2CPP) only through
exported functions and Unity's own internal calls, looked up by name. That means no memory offsets, so it isn't tied to
one exact game build.

Every frame, after the game has placed its camera, the mod renders that camera once per eye with the headset's view and
projection. It does this into its own render textures, using Unity's `Camera.Render()`. The game's UI cameras draw into
a separate transparent texture. A built-in OpenXR runtime thread, with its own D3D11 device, hands both to the headset
at its refresh rate. Each image is submitted with the head pose it was rendered for, so the compositor can reproject it
smoothly.

Most of this is engine-level and not specific to NEO. [docs/UNITY_VR_NOTES.md](docs/UNITY_VR_NOTES.md) describes the
technique, the pitfalls, and what other Unity games would need.

## Building from source
The DLL is cross-compiled on Linux with clang-cl (zig 0.17 / clang 22) and lld-link, against the MSVC STL/CRT and the
Windows SDK 10.0.28000:

```
tools/setup_toolchain.sh /path/to/toolchain64bit.zip   # once: MSVC + Windows SDK headers/libs (see the script)
./build.sh                                             # -> dist/version.dll
```

Layout: `src/loader` (version.dll proxy), `src/unity` (IL2CPP / UnityEngine access by name), `src/vr` (stereo
rendering, OpenXR runtime, performance timing), `src/common` (log, config), `vrkit/` (in-headset menu on Dear ImGui),
`third_party/` (MinHook, OpenXR SDK).

## Licence
GPL-3.0. See [LICENSE](LICENSE). Bundled third-party components keep their own licences. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

This is an unofficial fan mod and isn't affiliated with or endorsed by Square Enix. NEO: The World Ends with You is a
trademark of Square Enix. You need to own the game.
