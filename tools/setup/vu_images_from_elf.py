"""The VU1 program images without a census run: the code of each image lives in pieces in the executable, so a recipe of
offsets (no game data) rebuilds them on the player's machine; tools/vurecomp then lifts them to gen_zero/ps2_vu1_progs.inc.
  make a recipe (developer, once):   python -I vu_images_from_elf.py make  ELF CENSUS_DIR RECIPE.json
  rebuild the images (player):       python -I vu_images_from_elf.py build ELF RECIPE.json OUTDIR
The census folder (out/vu_programs) holds 16384-byte snapshots of VU1 micro memory; only code_lo..code_hi is used by vurecomp.
The recipe is names, offsets and sizes, nothing of the game. `build` checks a SHA-256 of each rebuilt code range."""
import hashlib
import json
import os
import sys


def longest_match(elf, code, i):
    """the longest piece of code[i:] found in elf, on an 8-byte boundary of the instruction stream: (elf offset, length)"""
    best_off, best_len = -1, 0
    n = 8
    while i + n <= len(code):
        off = elf.find(code[i:i + n])
        if off < 0:
            break
        best_off, best_len = off, n
        n *= 2
    if best_len == 0:
        return -1, 0
    # grow from the last hit, then in eights
    lo, hi = best_len, min(best_len * 2, len(code) - i)
    while hi - lo >= 8:
        mid = ((lo + hi) // 2) & ~7
        if mid <= lo:
            break
        off = elf.find(code[i:i + mid])
        if off >= 0:
            best_off, best_len = off, mid
            lo = mid
        else:
            hi = mid
    while i + best_len + 8 <= len(code) and elf[best_off:best_off + best_len + 8] == code[i:i + best_len + 8]:
        best_len += 8
    return best_off, best_len


def make(elf_path, census, recipe_path):
    elf = open(elf_path, "rb").read()
    man = json.load(open(os.path.join(census, "manifest.json"), encoding="utf-8"))
    out = {"elf_sha256": hashlib.sha256(elf).hexdigest(), "programs": []}
    ok = True
    for p in man["programs"]:
        img = open(os.path.join(census, p["image"]), "rb").read()
        lo, hi = p["code_lo"], p["code_hi"]
        code = img[lo:hi]
        i = 0
        pieces = []
        zeros = 0
        while i < len(code):
            if code[i:i + 8] == bytes(8):                   # a gap of never-written micro memory
                j = i
                while j < len(code) and code[j:j + 8] == bytes(8):
                    j += 8
                pieces.append({"vu": lo + i, "zero": j - i})
                zeros += j - i
                i = j
                continue
            off, n = longest_match(elf, code, i)
            if n == 0:
                print("  %s: no piece at vu %04X" % (p["crc"], lo + i))
                ok = False
                i += 8
                continue
            pieces.append({"vu": lo + i, "elf": off, "n": n})
            i += n
        q = {k: p[k] for k in ("crc", "image", "bytes", "code_lo", "code_hi", "activations", "instructions", "xgkicks", "entries")}
        q["pieces"] = pieces
        q["code_sha256"] = hashlib.sha256(code).hexdigest()
        out["programs"].append(q)
        print("%s  code %5d bytes: %3d pieces, %d zero bytes" % (p["crc"], len(code), len(pieces), zeros))
    out["dropped"] = man.get("dropped", 0)
    with open(recipe_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=0)
    print("recipe written, %d bytes; %s" % (os.path.getsize(recipe_path), "complete" if ok else "INCOMPLETE"))
    return 0 if ok else 1


def build(elf_path, recipe_path, outdir):
    elf = open(elf_path, "rb").read()
    rec = json.load(open(recipe_path, encoding="utf-8"))
    if hashlib.sha256(elf).hexdigest() != rec["elf_sha256"]:
        print("this is not the executable the recipe was made for")
        return 1
    os.makedirs(outdir, exist_ok=True)
    man = {"programs": [], "dropped": rec.get("dropped", 0)}
    for p in rec["programs"]:
        img = bytearray(p["bytes"])
        for q in p["pieces"]:
            if "zero" in q:
                continue
            img[q["vu"]:q["vu"] + q["n"]] = elf[q["elf"]:q["elf"] + q["n"]]
        if hashlib.sha256(bytes(img[p["code_lo"]:p["code_hi"]])).hexdigest() != p["code_sha256"]:
            print("%s: the rebuilt code differs" % p["crc"])
            return 1
        with open(os.path.join(outdir, p["image"]), "wb") as f:
            f.write(img)
        man["programs"].append({k: p[k] for k in ("crc", "image", "bytes", "code_lo", "code_hi", "activations", "instructions", "xgkicks", "entries")})
    with open(os.path.join(outdir, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(man, f, indent=1)
    print("%d images rebuilt in %s" % (len(rec["programs"]), outdir))
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "make":
        sys.exit(make(sys.argv[2], sys.argv[3], sys.argv[4]))
    if len(sys.argv) == 5 and sys.argv[1] == "build":
        sys.exit(build(sys.argv[2], sys.argv[3], sys.argv[4]))
    print(__doc__)
    sys.exit(2)
