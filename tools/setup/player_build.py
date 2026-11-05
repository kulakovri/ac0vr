"""player_build.py -- the build step of the player's first run, from the bundle made by tools/setup/make_bundle.sh: no MSYS2, no
CMake, no shell, no Vulkan SDK. Prototype (night_setup 2026-11-04; the setup program with its progress window is R4's next step).

  python -I player_build.py --bundle DIR --elf SLUS_213.46 --out OUT [--jobs N]

1. ELF -> C: tools/ps2recomp with the config in the bundle, into OUT/gen_zero (about 20 s, one thread).
2. VU1 programs -> C: the images rebuilt from the ELF (vu_images_from_elf.py, a recipe of offsets), tools/vurecomp.
3. C -> objects: gen_zero/*.c and runtime/src/ps2_vu.c (the one runtime file that includes generated code), through ninja.
4. Link with the prebuilt runtime library (it holds nothing of the game), copy the DLLs and the shaders next to the exe.
The result is OUT/build/zero/ac5.exe, OUT/gen_zero/ps2_image.bin. It prints the time of every stage."""
import argparse
import ctypes
import glob
import os
import shutil
import subprocess
import sys
import time


def ninja_escape(p):
    return p.replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def posix(p):
    return p.replace("\\", "/")


def ascii_path(p):
    """ninja and the compilers take the Windows code page: a folder name outside it goes by its short (8.3) name"""
    if p.isascii():
        return p
    buf = ctypes.create_unicode_buffer(4096)
    n = ctypes.windll.kernel32.GetShortPathNameW(p, buf, 4096)
    return buf.value if 0 < n < 4096 and buf.value.isascii() else p


def main():
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--bundle", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    bundle = ascii_path(os.path.abspath(args.bundle))
    out = ascii_path(os.path.abspath(args.out))
    gen = os.path.join(out, "gen_zero")
    exe_dir = os.path.join(out, "build", "zero")
    os.makedirs(gen, exist_ok=True)
    os.makedirs(exe_dir, exist_ok=True)
    tc_info = {}
    with open(os.path.join(bundle, "bundle.txt"), encoding="utf-8") as f:
        for line in f:
            if "=" in line:
                k, v = line.strip().split("=", 1)
                tc_info[k] = v
    sys.path.insert(0, os.path.join(bundle, "recomp"))
    t0 = time.time()

    # 1. ELF -> C
    import ps2recomp.__main__ as rec
    cfg = os.path.join(bundle, "recomp", "config_zero")
    rc = rec.main([os.path.abspath(args.elf), "-o", gen,
                   "--ida-db", os.path.join(cfg, "ida_like_zero.json"),
                   "--ida-seeds", os.path.join(cfg, "seeds_zero.json"),
                   "--symbols", os.path.join(cfg, "sdk", "sdk_symbols.json"),
                   "--symbols", os.path.join(cfg, "sdk", "manual_symbols.json"),
                   "--overrides", os.path.join(cfg, "sdk", "overrides_draft.json")])
    if rc:
        return rc
    t1 = time.time()
    print("stage ELF -> C: %.1f s" % (t1 - t0), flush=True)

    # 2. VU1 programs -> C
    import vu_images_from_elf as vui
    images = os.path.join(out, "vu_images")
    if vui.build(os.path.abspath(args.elf), os.path.join(bundle, "recomp", "vu_recipe.json"), images):
        return 1
    import vurecomp.__main__ as vur
    rc = vur.main(["--programs", images, "-o", os.path.join(gen, "ps2_vu1_progs.inc")])
    if rc:
        return rc
    t2 = time.time()
    print("stage VU1 -> C: %.1f s" % (t2 - t1), flush=True)

    # 3 and 4: build.ninja, ninja
    tc = os.path.join(bundle, "toolchain")
    bin_dir = os.path.join(tc, tc_info["bindir"])
    cc = os.path.join(bin_dir, tc_info["cc"])
    cxx = os.path.join(bin_dir, tc_info["cxx"])
    ninja = os.path.join(bin_dir, tc_info.get("ninja", "ninja.exe"))
    inc = '-I"%s" -I"%s"' % (posix(os.path.join(bundle, "runtime", "include")), posix(gen))
    common = tc_info.get("cflags", "") + " -std=gnu23 -w -fno-strict-aliasing -fwrapv " + inc
    objs = []
    # ninja hands these bytes to CreateProcessA: the Windows code page, not UTF-8 (a player's folder may be Cyrillic)
    with open(os.path.join(out, "build.ninja"), "w", encoding="mbcs", errors="strict", newline="\n") as f:
        # gen_opt= in bundle.txt: the generated code's optimisation level (CMake's PS2_GEN_OPT), -O1 by default
        f.write('rule gen\n  command = "%s" %s %s -fpatchable-function-entry=8 -MD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  description = C $in\n' % (posix(cc), common, tc_info.get("gen_opt", "-O1")))
        f.write('rule hot\n  command = "%s" %s -O3 -MD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  description = C $in\n' % (posix(cc), common))
        libs = posix(os.path.join(bundle, "runtime", "lib"))
        whole = posix(os.path.join(libs, "libac5rt.a"))
        f.write('rule link\n  command = "%s" -o $out $in -Wl,--whole-archive "%s" -Wl,--no-whole-archive "%s/liblua54.a" -L"%s" -lopenxr_loader -lSDL3 -lvulkan-1 -lm -lpthread -static-libstdc++ -static-libgcc %s\n  description = link $out\n' % (
            posix(cxx), whole, libs, libs, tc_info.get("ldflags", "")))
        sources = sorted(glob.glob(os.path.join(gen, "ps2_code_*.c")))
        for extra in ("ps2_func_table.c", "ps2_image.c", "ps2_symbols.c"):
            sources.append(os.path.join(gen, extra))
        obj_dir = os.path.join(out, "obj")
        os.makedirs(obj_dir, exist_ok=True)
        for src in sources:
            o = os.path.join(obj_dir, os.path.basename(src) + ".o")
            f.write("build %s: gen %s\n" % (ninja_escape(posix(o)), ninja_escape(posix(src))))
            objs.append(o)
        vu = os.path.join(bundle, "runtime", "src", "ps2_vu.c")
        o = os.path.join(obj_dir, "ps2_vu.c.o")
        f.write("build %s: hot %s\n" % (ninja_escape(posix(o)), ninja_escape(posix(vu))))
        objs.append(o)
        exe = os.path.join(exe_dir, "ac5.exe")
        f.write("build %s: link %s\n" % (ninja_escape(posix(exe)), " ".join(ninja_escape(posix(x)) for x in objs)))
    env = dict(os.environ)
    env["PATH"] = bin_dir + os.pathsep + os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
    p = subprocess.run([ninja, "-C", out, "-j", str(args.jobs)], env=env)
    if p.returncode:
        return p.returncode
    t3 = time.time()
    print("stage compile + link at %d jobs: %.1f s" % (args.jobs, t3 - t2), flush=True)

    # the rest next to the exe
    for dll in glob.glob(os.path.join(bundle, "dll", "*.dll")):
        shutil.copy(dll, exe_dir)
    shaders = os.path.join(exe_dir, "shaders")
    os.makedirs(shaders, exist_ok=True)
    for spv in glob.glob(os.path.join(bundle, "spirv", "*.spv")):
        shutil.copy(spv, shaders)
    shutil.copytree(os.path.join(bundle, "data"), out, dirs_exist_ok=True)
    print("done: %s, %.1f MB; the whole build %.1f s" % (exe, os.path.getsize(exe) / 1e6, time.time() - t0), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
