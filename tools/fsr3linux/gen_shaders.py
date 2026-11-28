"""FSR 3 for Linux (s131): compile the FSR 3.1 upscaler's Vulkan shaders into permutation headers with AMD's
FidelityFX shader compiler (FidelityFX SDK v1.1.4, MIT; Windows tool), the same arguments the SDK's CMake uses
(sdk/src/backends/vk/CMakeShadersFSR3Upscaler.txt, sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt).

    python tools/fsr3linux/gen_shaders.py <sdk dir (ffx-src-1.1.4/sdk)> <out dir>

The headers are plain C arrays of SPIR-V: the Linux build compiles them with the SDK's C++ (see build_ffx.cmake).
"""
import concurrent.futures
import glob
import os
import subprocess
import sys

sdk, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
os.makedirs(out, exist_ok=True)
sc = os.path.join(sdk, "tools", "binary_store", "FidelityFX_SC.exe")
gpu = os.path.join(sdk, "include", "FidelityFX", "gpu")
base = ["-reflection", "-deps=gcc", "-DFFX_GPU=1",
        "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
        "-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2"]
api = ["-compiler=glslang", "-e", "CS", "--target-env", "vulkan1.2", "-S", "comp", "-Os", "-DFFX_GLSL=1"]
perm = ["-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}",
        "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}",
        "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}",
        "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}",
        "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}",
        "-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}"]
inc = ["-I" + gpu, "-I" + os.path.join(gpu, "fsr3upscaler")]
shaders = sorted(glob.glob(os.path.join(sdk, "src", "backends", "vk", "shaders", "fsr3upscaler", "*.glsl")))
assert len(shaders) == 10, shaders

jobs = []
for s in shaders:
    name = os.path.splitext(os.path.basename(s))[0]
    for suffix, half in (("", 0), ("_wave64", 0), ("_16bit", 1), ("_wave64_16bit", 1)):  # GLSL: wave64 = same code
        jobs.append([sc] + base + api + perm + ["-name=" + name + suffix, "-DFFX_HALF=%d" % half] + inc +
                    ["-output=" + out, s])


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=out)
    return cmd[-1], r.returncode, (r.stdout + r.stderr)[-600:]


with concurrent.futures.ThreadPoolExecutor(8) as ex:
    bad = [(s, rc, log) for s, rc, log in ex.map(run, jobs) if rc]
for s, rc, log in bad:
    print("FAILED", s, rc, log)
hdrs = glob.glob(os.path.join(out, "*_permutations.h"))
print(len(hdrs), "headers,", sum(os.path.getsize(h) for h in hdrs) // 1024, "KB")
sys.exit(1 if bad or len(hdrs) != 40 else 0)
