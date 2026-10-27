#!/bin/bash
# Cross-builds NEO VR on Linux: clang-cl (zig 0.17 / clang 22) + lld-link against MSVC STL + Windows SDK 10.0.28000.
# Toolchain: an MSVC STL/CRT + Windows SDK zip (lib/, include/, Lib/<sdk>/, Include/<sdk>/) prepared in $TC
# (default ~/tc) by tools/setup_toolchain.sh.
#   dist/version.dll   the mod (x64). Copy next to "NEO The World Ends with You.exe".
set -e
cd "$(dirname "$0")"
B=${TC:-$HOME/tc}/build.sh
mkdir -p build/x64 dist
CFLAGS="/O2 /Zi /W3 /Isrc /Ivrkit /Ivrkit/third_party/imgui /Ithird_party/minhook/include /Ithird_party/openxr/include -D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
 -Wno-unused-function -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-sign-compare -Wno-unused-variable \
 -Wno-unused-command-line-argument"
SRC="src/main.cpp src/loader/version_proxy.cpp src/common/log.cpp src/common/config.cpp \
 src/unity/il2cpp.cpp src/unity/unity.cpp src/unity/probe.cpp \
 src/vr/stereo.cpp src/vr/xrout.cpp src/vr/perf.cpp \
 vrkit/vrkit_options.cpp vrkit/vrkit_menu.cpp vrkit/vrkit_menu_core.cpp vrkit/vrkit_input.cpp \
 vrkit/third_party/imgui/imgui.cpp vrkit/third_party/imgui/imgui_draw.cpp vrkit/third_party/imgui/imgui_tables.cpp \
 vrkit/third_party/imgui/imgui_widgets.cpp vrkit/third_party/imgui/imgui_impl_dx11.cpp \
 third_party/minhook/src/buffer.c third_party/minhook/src/hook.c third_party/minhook/src/trampoline.c \
 third_party/minhook/src/hde/hde64.c"
$B x64 dist/version.dll $CFLAGS /LD /Fobuild/x64/ /Fdbuild/x64/version.pdb $SRC \
   /link /DEF:src/loader/version.def /DEBUG /PDB:build/x64/version.pdb third_party/openxr/lib/openxr_loader_static.lib \
   user32.lib advapi32.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib shlwapi.lib pathcch.lib ole32.lib shell32.lib
rm -f dist/version.lib dist/version.exp
ls -la dist
