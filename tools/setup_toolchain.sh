#!/bin/bash
# Sets up the Linux cross-build toolchain in $TC (default ~/tc) from a zip holding the MSVC STL/CRT (include/, lib/x64)
# and the Windows SDK 10.0.28000 (Include/, Lib/) - copy them from a Visual Studio / Windows SDK install.
#   tools/setup_toolchain.sh /path/to/toolchain64bit.zip
# Needs python3 + pip (installs zig 0.17 = clang 22, which the MSVC STL in the archive requires).
set -e
ZIP=${1:?path to toolchain64bit.zip}
TC=${TC:-$HOME/tc}
pip install -q --break-system-packages ziglang==0.17.0
mkdir -p "$TC" && cd "$TC" && unzip -q -o "$ZIP"
# The SDK is case-mixed and Linux is case-sensitive: add lowercase aliases plus every spelling headers #include.
python3 - <<'PY'
import os,re
for root in ("Include","Lib","include","lib"):
    for d,_,fs in os.walk(root):
        names=set(fs)
        for f in fs:
            for alt in {f.lower(), f[:-4]+".lib" if f.endswith(".Lib") else f}:
                if alt not in names: os.symlink(f, os.path.join(d,alt)); names.add(alt)
dirs=[d for r in ("Include/10.0.28000.0/ucrt","Include/10.0.28000.0/um","Include/10.0.28000.0/shared","include") for d,_,_ in os.walk(r)]
idx={(d,f.lower()):f for d in dirs for f in os.listdir(d)}
rx=re.compile(rb'#\s*include\s*[<"]([^>"]+)[>"]'); inc=set()
for d in dirs:
    for f in os.listdir(d):
        p=os.path.join(d,f)
        if os.path.isfile(p): inc|={m.decode(errors='ignore').replace('\\','/') for m in rx.findall(open(p,'rb').read())}
for i in inc:
    b=os.path.basename(i); s=os.path.dirname(i)
    for d in dirs:
        dd=os.path.join(d,s) if s else d; real=idx.get((dd,b.lower()))
        if real and real!=b and not os.path.exists(os.path.join(dd,b)): os.symlink(real,os.path.join(dd,b))
PY
cat > "$TC/build.sh" <<'WRAP'
#!/bin/bash
# usage: build.sh x64|x86 out.(exe|dll) [clang-cl args...] [/link linker args...]
set -e
TC=$(cd "$(dirname "$0")" && pwd)
ARCH=$1; OUT=$2; shift 2
Z=$(python3 -c "import ziglang,os;print(os.path.dirname(ziglang.__file__))")
SDK=10.0.28000.0
if [ "$ARCH" = x64 ]; then TGT=x86_64-pc-windows-msvc; LA=x64; else TGT=i686-pc-windows-msvc; LA=x86; fi
INC="/imsvc $TC/include /imsvc $TC/Include/$SDK/ucrt /imsvc $TC/Include/$SDK/um /imsvc $TC/Include/$SDK/shared"
LIBP="/libpath:$TC/lib/$LA /libpath:$TC/Lib/$SDK/ucrt/$LA /libpath:$TC/Lib/$SDK/um/$LA"
# split at /link
CARGS=(); LARGS=(); seen=0
for a in "$@"; do if [ "$a" = /link ]; then seen=1; continue; fi; if [ $seen = 1 ]; then LARGS+=("$a"); else CARGS+=("$a"); fi; done
"$Z/zig" clang --driver-mode=cl -resource-dir "$Z/lib" --target=$TGT -fuse-ld=lld /MT /EHsc /std:c++20 -Wno-microsoft-anon-tag $INC "${CARGS[@]}" /Fe$OUT /link $LIBP "${LARGS[@]}"
WRAP
chmod +x "$TC/build.sh"; echo "toolchain ready in $TC"
