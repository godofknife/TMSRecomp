"""First-run setup: make the game data folder and game.dll from the player's OWN game files (plan phase 2).

    python setup.py --nsp <game.nsp> --keys <prod.keys> --data <new data folder> [--no-shaders]
    python setup.py --folder <folder with exefs\\ and romfs\\> [--no-shaders]
    python setup.py --dlss-only      (only the optional NVIDIA DLSS download; --dlss adds it to a full setup)

Linux: the same steps with tools from the system (clang + lld, nstool, dotnet or a self-contained NxShader): game.so
next to tms (--install, default as on Windows), nstool from --nstool, <setup>/nstool or PATH. No DLSS download.

Release layout (tools/package/package.ps1 -Setup): <install>\\tms.exe, <install>\\shaderc\\, and <install>\\setup\\ with
python\\, llvm\\ (clang.exe, lld-link.exe), nstool.exe, sdk\\ (guest.h, tms.lib, CRT import libs, game.def) and tools\\
(this folder's siblings, same layout as the repo's tools\\). Nothing of the game is shipped: every step runs here.

Steps: extract the program NCA (NSP only) -> check the game version -> link.py -> lift.py -> build_game.py (game.dll
next to tms.exe) -> shaderscan.py (all shaders into <data>\\cache\\shaders.bin). A finished step leaves a marker in
<data>\\setup\\, so a cancelled or failed run continues where it stopped. The launcher reads stdout:
"STEP i n text", "PROGRESS done total", "DATA <folder>", "ERROR text", "DONE".
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import threading
import time
import urllib.request

LINUX = sys.platform.startswith("linux")
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
SETUP_ROOT = os.path.dirname(TOOLS)
INSTALL = os.path.dirname(SETUP_ROOT)

# The hooks use fixed addresses of this exact build: Tokyo Mirage Sessions #FE Encore, title 0100A9400C9C2000,
# base game (no update). Other builds need their own address tables.
SUPPORTED = {
    "main": "4f097186eb73edc5fdc9f804393a82c42d35e803f5c59fcddcac002bc77aa113",
    "sdk": "4fe101e041c3018dde03cd7efd31a4d53397ce7bb5745a359def874c90a546ce",
}
SUPPORTED_TEXT = "Tokyo Mirage Sessions #FE Encore, title 0100A9400C9C2000, base game without update"


# NVIDIA DLSS (optional, the player ticks it on the Setup page): the release holds nothing of NVIDIA, this downloads NVIDIA's
# own signed nvngx_dlss.dll from NVIDIA's DLSS repository to the player's PC, pinned and hash-checked. The driver's NGX loads
# it from the folder of tms.exe (runtime/src/vk_dlss.cpp). NVIDIA's licence applies to that file.
# Linux (s129): NVIDIA's libnvidia-ngx-dlss.so of the same version, next to tms; the driver's libnvidia-ngx.so.1 loads it.
DLSS_VERSION = "310.9.16"
if sys.platform.startswith("linux"):
    DLSS_DLL = f"libnvidia-ngx-dlss.so.{DLSS_VERSION}"
    DLSS_URL = f"https://raw.githubusercontent.com/NVIDIA/DLSS/v{DLSS_VERSION}/lib/Linux_x86_64/rel/{DLSS_DLL}"
    DLSS_SIZE = 59481944
    DLSS_SHA256 = "30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6"
else:
    DLSS_DLL = "nvngx_dlss.dll"
    DLSS_URL = f"https://raw.githubusercontent.com/NVIDIA/DLSS/v{DLSS_VERSION}/lib/Windows_x86_64/rel/{DLSS_DLL}"
    DLSS_SIZE = 58956912
    DLSS_SHA256 = "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"


def has_rtx():
    if sys.platform.startswith("linux"):  # the NVIDIA driver's own GPU list (no nvidia-smi needed)
        import glob
        names = []
        for f in glob.glob("/proc/driver/nvidia/gpus/*/information"):
            try:
                names += [l for l in open(f, encoding="utf-8", errors="replace") if l.startswith("Model:")]
            except OSError:
                pass
        return any("RTX" in n.upper() for n in names)
    r = subprocess.run(["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_VideoController).Name"],
                       capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
    return any("NVIDIA" in n.upper() and "RTX" in n.upper() for n in r.stdout.splitlines())


def get_dlss(install):
    """Never fails the setup: the game runs without DLSS (FSR 3 then). A DLSS file the player put there is kept."""
    target = os.path.join(install, DLSS_DLL)
    if os.path.isfile(target):
        say("dlss:", DLSS_DLL, "is already there, kept")
        return
    if not has_rtx():
        say("dlss: no NVIDIA RTX GPU found, nothing downloaded")
        return
    part = target + ".part"
    try:
        say("dlss:", DLSS_URL)
        h, done = hashlib.sha256(), 0
        with urllib.request.urlopen(DLSS_URL, timeout=60) as r, open(part, "wb") as f:
            while block := r.read(1 << 20):
                f.write(block)
                h.update(block)
                done += len(block)
                say("PROGRESS", done, DLSS_SIZE)
        if done != DLSS_SIZE or h.hexdigest() != DLSS_SHA256:
            raise OSError(f"{done} bytes, sha256 {h.hexdigest()[:16]}: not the expected file")
        os.replace(part, target)
    except Exception as e:  # any network or file error: warn only
        if os.path.exists(part):
            os.remove(part)
        say("dlss: download failed:", str(e)[:300], "(the game runs without DLSS)")


def say(*parts):
    print(*parts, flush=True)


def fail(msg):
    say("ERROR", msg)
    sys.exit(1)


def run(cmd, what, progress=False):
    """Run a tool; pass its "compile i/n" lines on as PROGRESS, keep the rest for the error message."""
    if cmd[0] == sys.executable:  # our Python steps in UTF-8 mode too: install / game folders may be non-ASCII (U+266F)
        cmd = [cmd[0], "-X", "utf8"] + cmd[1:]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    tail = []
    for line in p.stdout:
        line = line.rstrip()
        if progress and line.startswith("compile ") and "/" in line:
            i, n = line.split()[1].split("/")
            say("PROGRESS", i, n)
        tail = (tail + [line])[-20:]
    if p.wait():
        fail(f"{what} failed:\n" + "\n".join(tail))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def dir_size(d):
    n = 0
    for root, _, files in os.walk(d):
        for f in files:
            try:
                n += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return n


def watch_size(paths, total, stop):
    """PROGRESS from the bytes written so far (nstool prints none)."""
    while not stop.wait(1.0):
        say("PROGRESS", sum(dir_size(p) if os.path.isdir(p) else os.path.getsize(p) if os.path.exists(p) else 0
                            for p in paths) >> 20, total >> 20)


def extract_nsp(nstool, keys, nsp, data):
    tmp = os.path.join(data, "setup", "nca")
    if not os.path.isdir(tmp):
        # the whole NSP (nstool's single-file -x fails on Windows paths); all NCAs but the program one are small.
        # Into nca.part first: a cancelled copy is never taken for a finished one.
        say("STEP", 1, 6, "Copying the game out of the NSP")
        part = tmp + ".part"
        shutil.rmtree(part, ignore_errors=True)
        stop = threading.Event()
        t = threading.Thread(target=watch_size, args=([part], os.path.getsize(nsp), stop))
        t.start()
        r = subprocess.run([nstool, "-k", keys, "-x", "/", part, nsp], capture_output=True, text=True, errors="replace")
        stop.set(), t.join()
        if not os.path.isdir(part) or not any(n.endswith(".nca") for n in os.listdir(part)):
            fail("nstool could not read the NSP (is it a Switch NSP?). nstool said:\n" + (r.stdout + r.stderr)[-1500:])
        os.replace(part, tmp)
    # the program NCA (code + romfs) is by far the biggest
    sizes = {n: os.path.getsize(os.path.join(tmp, n)) for n in os.listdir(tmp)
             if n.endswith(".nca") and not n.endswith(".cnmt.nca")}
    prog = os.path.join(tmp, max(sizes, key=sizes.get))
    # eShop-style NSPs: NCAs encrypted with a title key whose ticket (.tik) is in the NSP. nstool does not take it from
    # there by itself (title.keys it reads only next to prod.keys), so give it the ticket. No ticket: standard crypto.
    tik = sorted(n for n in os.listdir(tmp) if n.lower().endswith(".tik"))
    tik = ["--tik", os.path.join(tmp, tik[0])] if tik else []
    for part, sub in (("/0", "exefs"), ("/1", "romfs")):
        dst = os.path.join(data, sub)
        shutil.rmtree(dst, ignore_errors=True)
        say("STEP", 1, 6, f"Extracting {sub} (needs prod.keys)")
        stop = threading.Event()
        t = threading.Thread(target=watch_size, args=([dst], os.path.getsize(prog) if sub == "romfs" else 1 << 20, stop))
        t.start()
        r = subprocess.run([nstool, "-k", keys] + tik + ["-x", part, dst, prog], capture_output=True, text=True, errors="replace")
        stop.set(), t.join()
        if not os.path.isdir(dst) or not os.listdir(dst):
            fail(f"nstool could not extract {sub}: wrong or old prod.keys"
                 + ("" if tik else ", or the NSP needs a title key (no ticket inside: put title.keys next to prod.keys)")
                 + "? nstool said:\n" + (r.stdout + r.stderr)[-1500:])
    get_icon(nstool, keys, tmp, [n for n in sizes if os.path.join(tmp, n) != prog], data, tik)
    shutil.rmtree(tmp, ignore_errors=True)


def get_icon(nstool, keys, tmp, ncas, data, tik=()):
    """The game's own icon from the control NCA -> <data>/icon.jpg (the launcher makes the window icons from it).
    Optional: any failure only means the default icon. An extracted folder has no control NCA: the launcher then crops
    one from the title screen picture the game saves on its first start."""
    try:
        for n in sorted(ncas, key=lambda n: os.path.getsize(os.path.join(tmp, n))):
            d = os.path.join(tmp, "control")
            shutil.rmtree(d, ignore_errors=True)
            subprocess.run([nstool, "-k", keys, *tik, "-x", "/", d, os.path.join(tmp, n)], capture_output=True, text=True,
                           errors="replace", timeout=120)
            for root, _, files in os.walk(d):
                icons = sorted(f for f in files if f.startswith("icon_") and f.endswith(".dat"))
                if icons:
                    pick = "icon_AmericanEnglish.dat" if "icon_AmericanEnglish.dat" in icons else icons[0]
                    shutil.copyfile(os.path.join(root, pick), os.path.join(data, "icon.jpg"))
                    say("icon: from", n, pick)
                    return
        say("icon: no control NCA with an icon")
    except Exception as e:
        say("icon: none (" + str(e)[:200] + ")")


# ---- launcher Guide page: optional extras, each into the data folder (never into romfs: the game files stay as dumped)
def install_mod(src, data):
    """A mod folder: .xdelta patches against the game's .cpk files (pack_000_map ... .xdelta, the usual Switch patch
    layout) -> <data>\\mods\\<folder name>\\romfs\\Pack\\<cpk>; or a folder that already holds romfs\\ -> copied."""
    name = os.path.basename(os.path.normpath(src))
    dst = os.path.join(data, "mods", name)
    if os.path.isdir(os.path.join(src, "romfs")):
        say("STEP", 1, 1, f"Copying mod {name}")
        shutil.copytree(os.path.join(src, "romfs"), os.path.join(dst, "romfs"), dirs_exist_ok=True)
        return
    pack = os.path.join(data, "romfs", "Pack")
    cpks = sorted((n[:-4] for n in os.listdir(pack) if n.endswith(".cpk")), key=len, reverse=True)
    patches = sorted(n for n in os.listdir(src) if n.lower().endswith(".xdelta"))
    if not patches:
        fail(f"{src} has no .xdelta files and no romfs folder: pick the folder you unpacked the patch into")
    plan = []
    for p in patches:
        cpk = next((c for c in cpks if p.lower().startswith(c.lower())), None)  # longest name first
        if not cpk:
            fail(f"{p}: no game archive matches its name (expected it to start with one of {', '.join(sorted(cpks))})")
        plan.append((p, cpk))
    if len({c for _, c in plan}) != len(plan):
        fail("two patches are for the same game archive: keep only one per archive in the folder")
    sys.path.insert(0, TOOLS)
    import vcdiff
    out_dir = os.path.join(dst, "romfs", "Pack")
    os.makedirs(out_dir, exist_ok=True)
    for i, (p, cpk) in enumerate(plan, 1):
        say("STEP", i, len(plan), f"Patching {cpk}.cpk")
        out = os.path.join(out_dir, cpk + ".cpk")
        try:
            vcdiff.apply(os.path.join(pack, cpk + ".cpk"), os.path.join(src, p), out + ".part",
                         progress=lambda done, total: say("PROGRESS", done, total))
        except (ValueError, OSError) as e:
            if os.path.exists(out + ".part"):
                os.remove(out + ".part")
            fail(f"{p}: {e}")
        os.replace(out + ".part", out)
    # A newer version of a patch replaces the older one (user 11-24): other mods that are nothing but patched archives,
    # all of them archives this install just wrote, are deleted. Mods with anything else (textures, other files) stay.
    mine = {c.lower() + ".cpk" for _, c in plan}
    mods = os.path.join(data, "mods")
    for other in sorted(os.listdir(mods)):
        od = os.path.join(mods, other)
        if other == name or not os.path.isdir(od):
            continue
        files = [os.path.relpath(os.path.join(r, n), od).lower() for r, _, ns in os.walk(od) for n in ns]
        packed = {os.path.basename(f) for f in files if os.path.dirname(f) == os.path.join("romfs", "pack")}
        if files and len(packed) == len(files) and packed <= mine:
            say("STEP", 1, 1, f"Removing the older patch {other}")
            shutil.rmtree(od)


def install_zip(z, data):
    """A TMSRecomp extra ZIP (UHD textures: textures\\pack, 4K movies: movie_hd\\) unpacked into the data folder."""
    import zipfile
    with zipfile.ZipFile(z) as f:
        items = [i for i in f.infolist() if not i.is_dir() and not os.path.basename(i.filename).upper().startswith("README")]
        if not items or any(not i.filename.startswith(("textures/pack/", "movie_hd/")) or ".." in i.filename for i in items):
            fail(f"{os.path.basename(z)} is not a TMSRecomp UHD or MovieHD ZIP")
        total, done = sum(i.file_size for i in items), 0
        need = total - sum(os.path.getsize(os.path.join(data, i.filename)) for i in items if os.path.exists(os.path.join(data, i.filename)))
        if shutil.disk_usage(data).free < need + (1 << 30):
            fail(f"not enough disk space: {need >> 30} GB needed, {shutil.disk_usage(data).free >> 30} GB free")
        say("STEP", 1, 1, f"Unpacking {os.path.basename(z)}")
        import time
        for i in items:
            out = os.path.join(data, i.filename)
            stamp = time.mktime(i.date_time + (0, 0, -1))
            # a re-run continues: same size and the ZIP's own time = this file (size alone kept old files of a new pack)
            if not (os.path.exists(out) and os.path.getsize(out) == i.file_size and abs(os.path.getmtime(out) - stamp) < 3):
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with f.open(i) as s, open(out + ".part", "wb") as d:
                    shutil.copyfileobj(s, d, 1 << 22)
                os.replace(out + ".part", out)
                os.utime(out, (stamp, stamp))
            done += i.file_size
            say("PROGRESS", done >> 20, total >> 20)
        # a newer pack replaces the older one: its files the new ZIP no longer has are deleted (only in the folders it fills)
        keep = {os.path.normcase(os.path.join(data, i.filename)) for i in items}
        for top in {i.filename.split("/")[0] + "/" + i.filename.split("/")[1] for i in items if i.filename.count("/") >= 2}:
            for r, _, ns in os.walk(os.path.join(data, top)):
                for n in ns:
                    p = os.path.join(r, n)
                    if os.path.normcase(p) not in keep and not n.upper().startswith("README"):
                        os.remove(p)


def install_music(src, data):
    """Every .adx below the folder -> <data>\\music (custom battle music, ext_bgm.cpp)."""
    files = [os.path.join(r, n) for r, _, ns in os.walk(src) for n in ns if n.lower().endswith(".adx")]
    if not files:
        fail(f"{src} has no .adx files")
    dst = os.path.join(data, "music")
    os.makedirs(dst, exist_ok=True)
    say("STEP", 1, 1, f"Copying {len(files)} music files")
    for k, p in enumerate(files, 1):
        shutil.copy2(p, os.path.join(dst, os.path.basename(p)))
        say("PROGRESS", k, len(files))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nsp")
    ap.add_argument("--keys")
    ap.add_argument("--folder", help="already extracted game: becomes the data folder")
    ap.add_argument("--data", help="new data folder (NSP mode)")
    ap.add_argument("--install", default=INSTALL, help="folder with tms.exe (game.dll is written there)")
    ap.add_argument("--no-shaders", action="store_true")
    ap.add_argument("--dlss", action="store_true", help="also download NVIDIA's nvngx_dlss.dll (RTX GPUs)")
    ap.add_argument("--dlss-only", action="store_true", help="only that download")
    ap.add_argument("--mod", help="extra: mod folder (.xdelta patches or romfs\\) -> <data>\\mods\\<folder name>")
    ap.add_argument("--zip", help="extra: TMSRecomp UHD / MovieHD ZIP -> unpacked into <data>")
    ap.add_argument("--music", help="extra: folder with .adx battle music -> <data>\\music")
    ap.add_argument("--nstool", help="nstool program (default: next to this setup, else on PATH)")
    ap.add_argument("--clang", help="Linux: clang (default: on PATH)")
    a = ap.parse_args()
    for arg, f in ((a.mod, install_mod), (a.zip, install_zip), (a.music, install_music)):
        if arg:
            if not (a.data and os.path.isdir(os.path.join(a.data, "romfs"))):
                fail("set up the game first (the extras go into its game files folder)")
            f(os.path.abspath(arg), os.path.abspath(a.data))
            say("DONE")
            return
    if a.dlss_only:
        say("STEP", 1, 1, "Downloading NVIDIA DLSS")
        get_dlss(a.install)
        say("DONE")
        return
    if a.nsp:
        if not (a.keys and a.data):
            fail("NSP mode needs --keys (your prod.keys) and --data (where the game files go)")
        data = a.data
    elif a.folder:
        data = a.folder
    else:
        fail("give --nsp + --keys + --data, or --folder")
    data = os.path.abspath(data)
    os.makedirs(os.path.join(data, "setup"), exist_ok=True)
    py = sys.executable
    nstool = a.nstool or os.path.join(SETUP_ROOT, "nstool.exe")
    llvm = os.path.join(SETUP_ROOT, "llvm")
    sdk = os.path.join(SETUP_ROOT, "sdk")
    if LINUX:
        nstool = a.nstool or (os.path.join(SETUP_ROOT, "nstool") if os.path.isfile(os.path.join(SETUP_ROOT, "nstool"))
                              else shutil.which("nstool") or "nstool")
        if not os.path.isfile(os.path.join(sdk, "guest.h")):  # a source checkout: the repo's own header
            sdk = os.path.join(os.path.dirname(TOOLS), "runtime", "include")
    shaderc = os.path.join(a.install, "shaderc", "NxShader.dll")
    if not os.path.exists(shaderc) and os.path.exists(os.path.join(data, "shaderc", "NxShader.dll")):
        shaderc = os.path.join(data, "shaderc", "NxShader.dll")  # a source build: <data>/shaderc, where tms also looks
    # a step is redone when its marker is missing or older than its tool (an update re-runs only what changed)
    def done(step, *deps):
        m = os.path.join(data, "setup", step + ".done")
        return os.path.exists(m) and all(os.path.getmtime(m) >= os.path.getmtime(d) for d in deps)
    def mark(step):
        with open(os.path.join(data, "setup", step + ".done"), "w") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S\n"))
    t0 = time.time()

    if a.nsp and not done("extract"):
        for p, what in ((a.nsp, "NSP"), (a.keys, "prod.keys"), (nstool, "nstool.exe")):
            if not os.path.isfile(p):
                fail(f"{what} not found: {p}")
        free = shutil.disk_usage(data).free
        need = os.path.getsize(a.nsp) * 2 + (2 << 30)  # program NCA copy + romfs, removed again after
        if free < need:
            fail(f"not enough disk space on the data drive: {need >> 30} GB needed, {free >> 30} GB free")
        extract_nsp(nstool, a.keys, a.nsp, data)
        mark("extract")

    say("STEP", 2, 6, "Checking the game version")
    for m, want in SUPPORTED.items():
        p = os.path.join(data, "exefs", m)
        if not os.path.isfile(p):
            fail(f"{p} is missing: the folder needs exefs\\ and romfs\\ from your game (see Setup help)")
        if sha256(p) != want:
            fail(f"exefs\\{m} is from another build of the game. Supported: {SUPPORTED_TEXT}.")
    if not os.path.isdir(os.path.join(data, "romfs", "Pack")):
        fail(f"{data}\\romfs\\Pack is missing: extract the romfs of your game there")

    if not done("link", os.path.join(TOOLS, "recomp", "link.py")):
        say("STEP", 3, 6, "Linking the game modules")
        run([py, os.path.join(TOOLS, "recomp", "link.py"), os.path.join(data, "exefs"), os.path.join(data, "link")], "link")
        mark("link")

    lifter = [os.path.join(TOOLS, "recomp", f) for f in ("lift.py", "a32.py", "vfp.py", "names.txt")]
    if not done("lift", os.path.join(data, "setup", "link.done"), *lifter):
        say("STEP", 4, 6, "Translating the game code to C")
        run([py, lifter[0], os.path.join(data, "link"), os.path.join(data, "gen")], "lift")
        mark("lift")

    if LINUX:
        say("STEP", 5, 6, "Compiling the game code (game.so)")
        # the tarball's own setup/llvm (clang 23, as on Windows: nothing to install, e.g. Steam Deck), else the system's
        own = [os.path.join(llvm, n) for n in ("clang", "ld.lld")]
        if all(os.access(p, os.X_OK) for p in own) and not a.clang:
            clang, lld = own
        else:
            clang, lld = a.clang or shutil.which("clang"), shutil.which("ld.lld")
        if not clang or not lld:
            fail("clang and lld are needed to compile the game code (install them with your package manager)")
        run([py, os.path.join(HERE, "build_game.py"), "--gen", os.path.join(data, "gen"),
             "--obj", os.path.join(data, "setup", "obj"), "--out", os.path.join(a.install, "game.so"),
             "--include", sdk, "--clang", clang, "--lld", lld], "compile", True)
    else:
        say("STEP", 5, 6, "Compiling the game code (game.dll)")
        run([py, os.path.join(HERE, "build_game.py"), "--gen", os.path.join(data, "gen"),
             "--obj", os.path.join(data, "setup", "obj"), "--out", os.path.join(a.install, "game.dll"),
             "--include", sdk, "--lib", os.path.join(sdk, "tms.lib"), "--lib", os.path.join(sdk, "nx_crt_math.lib"),
             "--lib", os.path.join(sdk, "nx_vcruntime.lib"), "--def", os.path.join(sdk, "game.def"),
             "--clang", os.path.join(llvm, "clang.exe"), "--lld", os.path.join(llvm, "lld-link.exe")], "compile", True)

    if not a.no_shaders and not done("shaders", shaderc, os.path.join(TOOLS, "shaderscan", "shaderscan.py")):
        say("STEP", 6, 6, "Translating all shaders (no shader stutter later)")
        if not shutil.which("dotnet") and not os.path.exists(os.path.join(os.path.dirname(shaderc), "libhostfxr.so" if LINUX else "hostfxr.dll")):
            fail("the .NET runtime is not installed (needed for the shader translator, see README)")
        run([py, os.path.join(TOOLS, "shaderscan", "shaderscan.py"), data, "--dll", shaderc, "--set-aside"], "shader scan")
        mark("shaders")

    if a.dlss:
        say("STEP", 6, 6, "Downloading NVIDIA DLSS")
        get_dlss(a.install)

    say("DATA", data)
    say(f"setup finished in {time.time() - t0:.0f} s")
    say("DONE")


if __name__ == "__main__":
    main()
