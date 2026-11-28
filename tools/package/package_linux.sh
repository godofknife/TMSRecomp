#!/usr/bin/env bash
# package_linux.sh <build dir> <out.tar.gz>: the Linux release tarball from a runtime build (tms + launcher), as
# .github/workflows/build.yml runs it. Like package.ps1: program + setup tools only, NO game code or data (game.so,
# the lifted C and translated shaders are made on the player's PC by setup/tools/setup/setup.py).
# Inputs from the environment (the workflow fetches them; pinned versions there):
#   NX_SDL3_LIB   libSDL3.so.0 to bundle (distros without SDL3 3.4)        NX_SDL3_LICENSE  its LICENSE.txt
#   NX_NSTOOL     nstool binary (built from source)                          NX_NSTOOL_LICENSE
#   NX_PYTHON     python-build-standalone "install_only" folder (bin/python3) with capstone installed
#   NX_SHADERC    self-contained linux-x64 publish of tools/shaderc          NX_RYU_LICENSE   Ryujinx LICENSE.txt
#   NX_FFX_LICENSE  FidelityFX SDK LICENSE.txt (when the build made libamd_fidelityfx_vk.so)
#   NX_LLVM       folder with clang + ld.lld (LLVM's Linux release; setup compiles game.so with them)  NX_LLVM_LICENSE
set -euo pipefail
build=$(realpath "$1"); out=$(realpath -m "$2")
root=$(realpath "$(dirname "$0")/../..")
stage=$(mktemp -d)/TMSRecomp
mkdir -p "$stage"

# program: tms + launcher find bundled libraries next to themselves ($ORIGIN); cairo / pango / Vulkan come from the system
for f in tms launcher; do
    cp "$build/$f" "$stage/"
    strip "$stage/$f"
    patchelf --set-rpath '$ORIGIN' "$stage/$f"
done
cp -L "$NX_SDL3_LIB" "$stage/libSDL3.so.0"
# FSR 3 (s131): AMD's FidelityFX library built from the SDK source (CMakeLists.txt NX_FFX_SRC); tms loads it at run time
if [ -f "$build/libamd_fidelityfx_vk.so" ]; then
    cp "$build/libamd_fidelityfx_vk.so" "$stage/" && strip "$stage/libamd_fidelityfx_vk.so"
fi
# libvpx: bundled too, its soname differs between distributions (libvpx.so.7 / .9 / .11 ...)
vpx=$(ldd "$build/tms" | awk '/libvpx\.so/ {print $3}')
[ -n "$vpx" ] && cp -L "$vpx" "$stage/$(basename "$vpx")"
cp "$root/data/subtitles_en.tsv" "$stage/"
cp -r "$root/data/resist_icons" "$stage/"
cp "$root/tools/package/README_linux.txt" "$stage/README.txt"
cp "$root/LICENSE" "$stage/"
cp -r "$NX_SHADERC" "$stage/shaderc"

# setup/: what the first-run setup needs to build game.so from the player's own game (bundled clang + lld compile it)
su="$stage/setup"
mkdir -p "$su/sdk" "$su/tools/recomp" "$su/tools/texextract" "$su/tools/shaderscan" "$su/tools/setup"
cp "$NX_NSTOOL" "$su/nstool"
mkdir -p "$su/llvm" && cp "$NX_LLVM/clang" "$NX_LLVM/ld.lld" "$su/llvm/"
cp -r "$NX_PYTHON" "$su/python"
rm -rf "$su"/python/lib/python3.*/config-3.*  # static lib + python.o for building C extensions: not needed
cp "$root/runtime/include/guest.h" "$su/sdk/"
cp "$root"/tools/recomp/{link.py,lift.py,a32.py,vfp.py,names.txt} "$su/tools/recomp/"
cp "$root"/tools/{nso.py,cpk.py,vcdiff.py} "$su/tools/"
cp "$root/tools/texextract/texextract.py" "$su/tools/texextract/"
clang -O2 -shared -fPIC -o "$su/tools/texextract/fasttex.so" "$root/tools/texextract/fasttex.c"
cp "$root/tools/shaderscan/shaderscan.py" "$su/tools/shaderscan/"
cp "$root"/tools/setup/{setup.py,build_game.py} "$su/tools/setup/"  # no tms.profdata: Windows' bundled clang only

# no game code or data in the tarball (same list as package.ps1)
bad=$(find "$stage" -type f \( -regextype posix-extended -regex '.*/(game\.(so|dll)|image\.bin|layout\.json|shaders\.bin|pipelines\.(bin|recipes)|files\.txt|funcs\.h|table\.c|f_[0-9]+\.c)' \
      -o -name '*.nca' -o -name '*.nsp' -o -name '*.o' -o -name '*.cpk' -o -name '*.keys' \) -print)
[ -z "$bad" ] || { echo "game code/data staged, refusing to package: $bad" >&2; exit 1; }
[ "$(stat -c %s "$stage/tms")" -lt $((8 << 20)) ] || { echo "tms is over 8 MB: does it contain game code?" >&2; exit 1; }

notice() { printf '==== %s\n\n' "$1"; cat "$2"; printf '\n\n'; }
{
    printf 'Third-party software in this package\n\n'
    notice "SDL3 (libSDL3.so.0), zlib licence" "$NX_SDL3_LICENSE"
    [ -f /usr/share/doc/libvpx9/copyright ] && notice "libvpx ($(basename "${vpx:-libvpx}")), BSD-3-Clause" /usr/share/doc/libvpx9/copyright
    notice "Ryujinx shader translator (shaderc/) and BC7 tables (in tms, runtime/src/bc7.h), MIT" "$NX_RYU_LICENSE"
    printf '==== .NET runtime (shaderc/, self-contained), MIT: https://github.com/dotnet/runtime/blob/main/LICENSE.TXT\n\n'
    notice "nstool (setup/nstool), MIT" "$NX_NSTOOL_LICENSE"
    [ -f "$stage/libamd_fidelityfx_vk.so" ] && [ -n "$NX_FFX_LICENSE" ] &&
        notice "AMD FidelityFX SDK 1.1.4 (libamd_fidelityfx_vk.so: FidelityFX API, FSR 3 upscaler, Vulkan backend), MIT" "$NX_FFX_LICENSE"
    notice "LLVM clang + lld (setup/llvm/), Apache-2.0 WITH LLVM-exception" "$NX_LLVM_LICENSE"
    notice "Python (setup/python/), PSF licence" "$(ls "$NX_PYTHON"/lib/python3.*/LICENSE.txt | head -1)"
    printf '==== Capstone (setup/python/.../site-packages/capstone), BSD-3-Clause: https://github.com/capstone-engine/capstone\n\n'
} > "$stage/THIRD_PARTY.txt"

tar -C "$(dirname "$stage")" -czf "$out.part" TMSRecomp
mv "$out.part" "$out"
echo "wrote $out ($(($(stat -c %s "$out") >> 20)) MB)"
