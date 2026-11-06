"""release_scan.py FOLDER [--elf SLUS_213.46] [--list FILE] -- nothing of the game and no machine path goes into a release (setup_1105). Run by
make_release.sh on its folder (the installer's and the update zip's files are in it); it refuses the release on any hit, exit status 1.

Names: SLUS, ps2_image.bin, gen_zero/, .cpit, .iso, memory cards, the textures made from the disc's pictures (public_manifest.txt keeps them out).
Contents, with --elf (the developer's own copy of the game's executable): 96 pieces of 24 bytes from all over the file, searched in every file:
a release that holds a part of the game's code or data, whole or as generated C's image, shows here. Strings of the C sources would not match:
this looks for the bytes.
Machine paths, in every file, text or binary: the developer's user name (from the environment and git, at run time, written nowhere; a hit prints
the file and the offset, never the name); drive paths into a user profile, in what we build (the compilers and Python hold their own builders').
--list FILE: scan the files named in FILE (one per line, relative to FOLDER) instead of the whole folder: the same scan for the installer's list."""
import argparse
import os
import re
import subprocess
import sys

BAD_NAME = re.compile(r"(?i)(^|/)(SLUS[_.-]|ps2_image\.bin|gen_zero(/|$)|[^/]*\.cpit$|[^/]*\.iso$|[^/]*\.ps2mc$|[^/]*\.elf$|3F861D67_)|(^|/)textures/(hud|menu|ps_modern)(/|$)")
PROFILE = re.compile(rb"(?i)[A-Z]:[/\\]{1,2}Users[/\\]{1,2}[A-Za-z0-9_.-]+|/c/Users/[A-Za-z0-9_.-]+|agent_trees|acvr_backup")
REPO = re.compile(rb"github\.com/(?:repos/)?[\w.-]+/ac0vr")
THEIRS = re.compile(r"(stage/)?(python/|bundle/toolchain/|bundle/dll/)")
SIZE_PIECE = 24
N_PIECES = 96


def person_patterns():
    words = set()
    for name in ("USERNAME", "USER"):
        v = os.environ.get(name)
        if v and len(v) >= 4:
            words.add(v)
    for key in ("user.name", "user.email"):
        try:
            v = subprocess.run(["git", "config", key], capture_output=True, text=True).stdout.strip()
        except OSError:
            v = ""
        if v and len(v) >= 4:
            words.add(v)
            # (a first name can be a word of the language too, in the compilers and in Python's Unicode tables: only the longer pieces)
            words.update(w for w in re.split(r"[\s@.]+", v) if len(w) >= 6 and w.lower() not in ("gmail", "users", "noreply"))
    if not words:
        return None
    return re.compile(b"|".join(re.escape(w.encode("utf-8")) for w in sorted(words)), re.I)


def elf_pieces(path):
    d = open(path, "rb").read()
    pieces = []
    step = max(1, (len(d) - SIZE_PIECE) // (N_PIECES * 4))
    for off in range(0x1000, len(d) - SIZE_PIECE, step):
        p = d[off:off + SIZE_PIECE]
        if len(set(p)) >= 12:                  # no zero runs, no padding: bytes that only the game has
            pieces.append(p)
        if len(pieces) >= N_PIECES:
            break
    return re.compile(b"|".join(re.escape(p) for p in pieces)) if pieces else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--elf")
    ap.add_argument("--list")
    args = ap.parse_args()
    root = os.path.abspath(args.folder)
    if args.list:
        with open(args.list, encoding="utf-8") as f:
            files = [line.strip().replace("\\", "/") for line in f if line.strip()]
    else:
        files = []
        for d, _, names in os.walk(root):
            files += [os.path.relpath(os.path.join(d, n), root).replace("\\", "/") for n in names]
    game = elf_pieces(args.elf) if args.elf else None
    person = person_patterns()
    hits = 0
    total = 0
    for rel in sorted(files):
        total += 1
        if BAD_NAME.search(rel):
            print("NAME %s" % rel)
            hits += 1
            continue
        path = os.path.join(root, rel)
        with open(path, "rb") as f:
            data = f.read()
        if game:
            m = game.search(data)
            if m:
                print("GAME BYTES %s at offset %d" % (rel, m.start()))
                hits += 1
        data = REPO.sub(b"", data)                     # the public repository's own address is meant to be there: the updater asks it
        for rx, kind in ((PROFILE, "PATH"), (person, "PERSON")):
            if rx is None:
                continue
            if kind == "PATH" and THEIRS.match(rel):   # other people's build folders, inside their binaries
                continue
            m = rx.search(data)
            if m:
                shown = m.group(0).decode("ascii", "replace") if kind == "PATH" else "(masked)"
                print("%s %s at offset %d: %s" % (kind, rel, m.start(), shown))
                hits += 1
    print("release_scan: %d files, %d hits%s" % (total, hits, "" if game else " (no --elf: names and paths only)"))
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
