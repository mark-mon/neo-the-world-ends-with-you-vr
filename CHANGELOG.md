# Changelog

## 1.0.0 — 2026-10-27
First release.
- Native per-eye stereo rendering of the game's world camera through OpenXR (in-process runtime, static OpenXR loader;
  ships as a single `version.dll`).
- Head tracking relative to the game camera, recentre (F9), optional level horizon and world scale.
- Game UI on its own HUD panel (world- or head-locked); full-screen menus on a larger panel.
- In-headset settings menu (vrkit / Dear ImGui): L3 + R3 or Insert, fixed in the room, saves to `config.txt`.
- Resolution presets: performance 70%, balanced 85%, native 100%, high 120%, ultra 140%, custom.
- Frame pacing to the headset refresh rate (fixes frame drops in busy battles); optional skip of the desktop 3D render.
- Diagnostics: `neovr.log`, per-second performance log (`perf_log`), headset image dump (F11), camera probe (`probe`).
