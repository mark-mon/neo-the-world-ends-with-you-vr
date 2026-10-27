# Third-party components

The mod's own code is licensed under GPL-3.0 (see LICENSE). It includes the following components under their own
licences, all compatible with GPL-3.0:

| Component | Where | Licence | Licence text |
|---|---|---|---|
| MinHook — Minimalistic API Hooking Library (Tsuda Kageyu) | `third_party/minhook` | BSD 2-Clause | `third_party/minhook/LICENSE.txt` |
| OpenXR SDK headers and static loader (The Khronos Group) | `third_party/openxr` | Apache-2.0 | `third_party/openxr/LICENSE` |
| Dear ImGui 1.91.5 (Omar Cornut) | `vrkit/third_party/imgui` | MIT | `vrkit/third_party/imgui/LICENSE.txt` |

The released `version.dll` statically links all three.
