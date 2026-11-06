"""player_build.py -- the build step of the player's PC, from the bundle made by tools/setup/make_bundle.sh: no MSYS2, no CMake, no shell,
no Vulkan SDK (night_setup 2026-11-04; the cache and the layout setup_1105 2026-11-05). ac0vr.exe runs it through the bundled Python.

  python -I -B player_build.py --bundle DIR --elf SLUS_213.46 --out OUT [--jobs N] [--release TEXT] [--force]

Three stages with a cache: each has a hash of its inputs in OUT/build_state.json, and a rerun does only the stages whose inputs changed.
1. recompile: tools/ps2recomp (ELF -> C), the VU1 images rebuilt from the ELF (vu_images_from_elf.py, a recipe of offsets), tools/vurecomp.
   Inputs: the ELF, the recompilers and their config. Output: OUT/build/zero/gen_zero; a file that comes out the same keeps its date, so
   a small recompiler change compiles only what it changed.
2. compile: gen_zero/*.c and runtime/src/ps2_vu.c (the one runtime file that includes generated code), through ninja. Inputs: gen_zero, ps2_vu.c,
   the runtime headers they really use (from the compiler's .d files), the toolchain's identity and flags.
3. link: with the prebuilt runtime library (it holds nothing of the game). Inputs: the objects, the runtime library, the SDK's import libraries.
   Linked beside the old exe and swapped at the end: a failure leaves the previous build runnable.
Then the DLLs, the shaders and the data next to the exe (only files that changed), and the release's version.txt beside it.

The result is OUT/build/zero/ac5.exe with everything it needs next to it (docs/SETTINGS.md "Paths"). A runtime-only release relinks in
seconds; a recompiler or config change costs the recompile and the compile. All paths the tools see are relative to OUT, so a folder
outside the Windows code page does not matter to ninja or the compilers.

Lines for ac0vr.exe's window, among the plain log lines: `@plan NAME,NAME`, `@stage NAME`, `@progress FRACTION`, `@done NAME SECONDS`,
`@total SECONDS`. `--check` prints `current` or the stages to run, and changes nothing."""
import argparse
import filecmp
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

STATE = "build_state.json"


def say(text):
    print(text, flush=True)


def posix(p):
    return p.replace("\\", "/")


def ninja_escape(p):
    return p.replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def hash_files(paths, extra=""):
    h = hashlib.sha256(extra.encode("utf-8"))
    for p in paths:
        h.update(posix(os.path.basename(p)).encode("utf-8", "replace") + b"\0")
        with open(p, "rb") as f:
            while True:
                b = f.read(1 << 20)
                if not b:
                    break
                h.update(b)
        h.update(b"\1")
    return h.hexdigest()


def tree_files(root, skip=("__pycache__",)):
    out = []
    for d, sub, files in os.walk(root):
        sub[:] = sorted(s for s in sub if s not in skip)
        out += [os.path.join(d, f) for f in sorted(files)]
    return out


def sync_file(src, dst):
    """copy when different; returns 1 when copied (a file that stays keeps its date: ninja and the pipeline cache's stamp look at it)"""
    if os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False):
        return 0
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    tmp = dst + ".new"
    shutil.copy2(src, tmp)
    os.replace(tmp, dst)
    return 1


def sync_tree(src, dst):
    n = 0
    for f in tree_files(src):
        n += sync_file(f, os.path.join(dst, os.path.relpath(f, src)))
    return n


def load_state(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_state(path, st):
    tmp = path + ".new"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(st, f, indent=1, ensure_ascii=False)
    os.replace(tmp, path)


def read_bundle_txt(path):
    info = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if "=" in line:
                k, v = line.strip().split("=", 1)
                info[k] = v
    return info


def recomp_hash(args, bundle):
    return hash_files(tree_files(os.path.join(bundle, "recomp")) + [args.elf])


def stage_recompile(args, bundle, gen):
    """into a fresh folder; only files that differ replace the old ones"""
    tmp = gen + ".new"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    sys.path.insert(0, os.path.join(bundle, "recomp"))
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import ps2recomp.__main__ as rec
    cfg = os.path.join(bundle, "recomp", "config_zero")
    rc = rec.main([args.elf, "-o", tmp,
                   "--ida-db", os.path.join(cfg, "ida_like_zero.json"),
                   "--ida-seeds", os.path.join(cfg, "seeds_zero.json"),
                   "--symbols", os.path.join(cfg, "sdk", "sdk_symbols.json"),
                   "--symbols", os.path.join(cfg, "sdk", "manual_symbols.json"),
                   "--overrides", os.path.join(cfg, "sdk", "overrides_draft.json")])
    if rc:
        return rc
    say("@progress 0.8")
    import vu_images_from_elf as vui
    images = tmp + ".vu"
    shutil.rmtree(images, ignore_errors=True)
    if vui.build(args.elf, os.path.join(bundle, "recomp", "vu_recipe.json"), images):
        return 1
    import vurecomp.__main__ as vur
    rc = vur.main(["--programs", images, "-o", os.path.join(tmp, "ps2_vu1_progs.inc")])
    if rc:
        return rc
    shutil.rmtree(images, ignore_errors=True)
    os.makedirs(gen, exist_ok=True)
    keep = set()
    changed = 0
    for f in tree_files(tmp):
        rel = os.path.relpath(f, tmp)
        keep.add(rel)
        changed += sync_file(f, os.path.join(gen, rel))
    for f in tree_files(gen):
        if os.path.relpath(f, gen) not in keep:
            os.remove(f)
    shutil.rmtree(tmp, ignore_errors=True)
    say("recompile: %d generated files changed" % changed)
    return 0


def gen_sources(gen):
    sources = sorted(glob.glob(os.path.join(gen, "ps2_code_*.c")))
    for extra in ("ps2_func_table.c", "ps2_image.c", "ps2_symbols.c"):
        sources.append(os.path.join(gen, extra))
    return sources


def write_ninja(path, bundle, tc_info, gen, obj_dir, exe_new):
    """relative paths only: ninja hands these bytes to CreateProcessA, in the Windows code page"""
    bin_dir = os.path.join(bundle, "toolchain", tc_info["bindir"])
    cc = os.path.join(bin_dir, tc_info["cc"])                # (backslashes: CreateProcess does not find a relative program by forward ones)
    cxx = os.path.join(bin_dir, tc_info["cxx"])
    inc = '-I"%s" -I"%s"' % (posix(os.path.join(bundle, "runtime", "include")), posix(gen))
    common = tc_info.get("cflags", "") + " -std=gnu23 -w -fno-strict-aliasing -fwrapv " + inc
    libs = posix(os.path.join(bundle, "runtime", "lib"))
    objs = []
    with open(path, "w", encoding="ascii", newline="\n") as f:
        # gen_opt= in bundle.txt: the generated code's optimisation level (CMake's PS2_GEN_OPT)
        # (-MF for used_headers and prune_bundle.py; no `depfile`: ninja would stat the compiler's own headers by absolute paths in the code page,
        # and in a folder outside it every object would stay dirty: 42 s at the link. compile_base decides when all objects go.)
        f.write('rule gen\n  command = "%s" %s %s -fpatchable-function-entry=8 -MD -MF $out.d -c $in -o $out\n  description = C $in\n' % (cc, common, tc_info.get("gen_opt", "-O1")))
        f.write('rule hot\n  command = "%s" %s -O3 -MD -MF $out.d -c $in -o $out\n  description = C $in\n' % (cc, common))
        # (ACZ_LINK_VERBOSE: make_release.sh's rehearsal: prune_bundle.py reads what the link opened)
        trace = " -Wl,--verbose" if os.environ.get("ACZ_LINK_VERBOSE") else ""
        f.write('rule link\n  command = "%s" -o $out $in -Wl,--whole-archive "%s/libac5rt.a" -Wl,--no-whole-archive "%s/liblua54.a" -L"%s" -lopenxr_loader -lSDL3 -lvulkan-1 -lws2_32 -lwinhttp -lm -lpthread -static-libstdc++ -static-libgcc %s%s\n  description = link $out\n' % (
            cxx, libs, libs, libs, tc_info.get("ldflags", ""), trace))
        for src in gen_sources(gen):
            o = os.path.join(obj_dir, os.path.basename(src) + ".o")
            f.write("build %s: gen %s\n" % (ninja_escape(posix(o)), ninja_escape(posix(src))))
            objs.append(o)
        vu = os.path.join(bundle, "runtime", "src", "ps2_vu.c")
        o = os.path.join(obj_dir, "ps2_vu.c.o")
        f.write("build %s: hot %s\n" % (ninja_escape(posix(o)), ninja_escape(posix(vu))))
        objs.append(o)
        f.write("build compile: phony %s\n" % " ".join(ninja_escape(posix(x)) for x in objs))
        f.write("build %s: link %s\n" % (ninja_escape(posix(exe_new)), " ".join(ninja_escape(posix(x)) for x in objs)))
        f.write("default compile\n")
    return objs


def run_ninja(ninja, ninja_file, target, jobs, env):
    """ninja's `[n/m] ...` lines become @progress"""
    p = subprocess.Popen([ninja, "-f", ninja_file, "-j", str(jobs), target], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         encoding="utf-8", errors="replace")
    rx = re.compile(r"^\[(\d+)/(\d+)\]")
    for line in p.stdout:
        line = line.rstrip()
        m = rx.match(line)
        if m and int(m.group(2)):
            say("@progress %.3f" % (int(m.group(1)) / int(m.group(2))))
        say(line)
    return p.wait()


def used_headers(obj_dir, include_dir):
    """the runtime headers the objects really include: an update that changes another one is no reason to compile"""
    inc = os.path.normcase(os.path.abspath(include_dir))
    used = set()
    for d in glob.glob(os.path.join(obj_dir, "*.d")):
        with open(d, encoding="utf-8", errors="replace") as f:
            text = f.read().replace("\\\n", " ")
        for tok in re.split(r"(?<!\\)\s+", text):
            tok = tok.replace("\\ ", " ").rstrip(":")
            if tok and os.path.normcase(os.path.abspath(tok)).startswith(inc):
                used.add(posix(os.path.relpath(os.path.abspath(tok), os.path.abspath(include_dir))))
    return sorted(used)


def main():
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--bundle", required=True)
    ap.add_argument("--elf")
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    ap.add_argument("--release", default="")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    if args.elf:
        args.elf = os.path.abspath(args.elf)
    bundle_abs = os.path.abspath(args.bundle)
    os.makedirs(args.out, exist_ok=True)
    os.chdir(args.out)
    same_drive = os.path.splitdrive(bundle_abs)[0].lower() == os.path.splitdrive(os.getcwd())[0].lower()
    bundle = os.path.relpath(bundle_abs) if same_drive else bundle_abs
    exe_dir = os.path.join("build", "zero")
    gen = os.path.join(exe_dir, "gen_zero")
    obj_dir = os.path.join("build", "obj")
    os.makedirs(obj_dir, exist_ok=True)
    os.makedirs(exe_dir, exist_ok=True)
    tc_info = read_bundle_txt(os.path.join(bundle, "bundle.txt"))
    state = load_state(STATE)
    stages = state.get("stages", {}) if not args.force else {}
    t0 = time.time()
    exe = os.path.join(exe_dir, "ac5.exe")
    exe_new = os.path.join(exe_dir, "ac5.new.exe")
    ninja_file = os.path.join("build", "build.ninja")
    tc_id = "|".join(tc_info.get(k, "") for k in ("kind", "toolchain_id", "cc", "cflags", "gen_opt"))

    def compile_inputs():
        vu = os.path.join(bundle, "runtime", "src", "ps2_vu.c")
        inc = os.path.join(bundle, "runtime", "include")
        heads = stages.get("compile", {}).get("headers")
        heads = [os.path.join(inc, h) for h in heads if os.path.exists(os.path.join(inc, h))] if heads else tree_files(inc)
        return hash_files(tree_files(gen) + [vu] + heads, tc_id)

    def compile_base():
        """what every object depends on, not only its own .c: when this changes, all objects go"""
        inc = os.path.join(bundle, "runtime", "include")
        heads = stages.get("compile", {}).get("headers")
        heads = [os.path.join(inc, h) for h in heads if os.path.exists(os.path.join(inc, h))] if heads else tree_files(inc)
        return hash_files(heads + [f for f in tree_files(gen) if not f.endswith((".c", ".bin"))], tc_id)

    def link_inputs():
        objs = sorted(glob.glob(os.path.join(obj_dir, "*.o")))
        lib = os.path.join(bundle, "runtime", "lib")
        return hash_files(objs + sorted(tree_files(lib)), tc_info.get("ldflags", "") + tc_id)

    # what has to run: a stage whose inputs changed, and everything after a stage that will run
    need = []
    if args.elf and os.path.exists(args.elf):
        if stages.get("recompile", {}).get("hash") != recomp_hash(args, bundle):
            need.append("recompile")
    elif not stages.get("recompile"):
        need.append("recompile")
    if not need:
        if not os.path.exists(os.path.join(gen, "ps2_func_table.c")):
            need.append("recompile")
        elif stages.get("compile", {}).get("hash") != compile_inputs():
            need.append("compile")
        elif stages.get("link", {}).get("hash") != link_inputs() or not os.path.exists(exe):
            need.append("link")
    if "recompile" in need:
        need += ["compile", "link"]
    elif "compile" in need:
        need.append("link")
    if args.check:
        say("current" if not need and state.get("release") == args.release else "run: " + ",".join(need or ["install"]))
        return 0
    if "recompile" in need and not (args.elf and os.path.exists(args.elf)):
        say("The game's executable is needed for this build (--elf).")
        return 2
    say("@plan " + ",".join(need))

    # 1
    if "recompile" in need:
        t = time.time()
        say("@stage recompile")
        rc = stage_recompile(args, bundle, gen)
        if rc:
            return rc
        stages["recompile"] = {"hash": recomp_hash(args, bundle), "seconds": round(time.time() - t, 1)}
        say("@done recompile %.1f" % (time.time() - t))

    # the compilers' own folder first: their DLLs; nothing of the player's PATH
    bin_dir = os.path.abspath(os.path.join(bundle, "toolchain", tc_info["bindir"]))
    env = dict(os.environ)
    env["PATH"] = bin_dir + os.pathsep + os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
    ninja = os.path.join(bin_dir, tc_info.get("ninja", "ninja.exe"))
    write_ninja(ninja_file, bundle, tc_info, gen, obj_dir, exe_new)

    # 2
    if "compile" in need:
        t = time.time()
        say("@stage compile")
        if stages.get("compile", {}).get("base") != compile_base():
            for o in glob.glob(os.path.join(obj_dir, "*.o")):
                os.remove(o)
        rc = run_ninja(ninja, ninja_file, "compile", args.jobs, env)
        if rc:
            return rc
        heads = used_headers(obj_dir, os.path.join(bundle, "runtime", "include"))
        stages["compile"] = {"headers": heads, "seconds": round(time.time() - t, 1)}
        stages["compile"]["hash"] = compile_inputs()
        stages["compile"]["base"] = compile_base()
        say("@done compile %.1f" % (time.time() - t))
        save_state(STATE, dict(state, stages=stages))

    # 3
    if "link" in need:
        t = time.time()
        say("@stage link")
        if os.path.exists(exe_new):
            os.remove(exe_new)
        rc = run_ninja(ninja, ninja_file, posix(exe_new), args.jobs, env)
        if rc:
            return rc
        stages["link"] = {"hash": link_inputs(), "seconds": round(time.time() - t, 1)}
        say("@done link %.1f" % (time.time() - t))

    # the rest next to the exe, after the link: a failure above leaves the old exe with the old shaders. Only now the exe is swapped.
    t = time.time()
    say("@stage install")
    if os.path.exists(exe_new):
        try:
            os.replace(exe_new, exe)
        except PermissionError:
            say("%s is in use: is the game still running? Close it and try again." % exe)
            return 3
    n = 0
    for dll in glob.glob(os.path.join(bundle, "dll", "*.dll")):
        n += sync_file(dll, os.path.join(exe_dir, os.path.basename(dll)))
    shaders = os.path.join(exe_dir, "shaders")
    keep = set()
    for spv in glob.glob(os.path.join(bundle, "spirv", "*.spv")):
        keep.add(os.path.basename(spv))
        n += sync_file(spv, os.path.join(shaders, os.path.basename(spv)))
    for spv in glob.glob(os.path.join(shaders, "*.spv")):
        if os.path.basename(spv) not in keep:
            os.remove(spv)
    n += sync_tree(os.path.join(bundle, "data"), exe_dir)
    if args.release:
        with open(os.path.join(exe_dir, "version.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(args.release + "\n")
    state["stages"] = stages
    state["release"] = args.release
    save_state(STATE, state)
    say("install: %d files changed" % n)
    say("@done install %.1f" % (time.time() - t))
    size = os.path.getsize(exe) / 1e6 if os.path.exists(exe) else 0
    say("done: %s, %.1f MB; this run %.1f s" % (exe, size, time.time() - t0))
    say("@total %.1f" % (time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
