"""Self-test of disc_check with fixtures (no game data): python -I tools/setup/test_disc_check.py path/to/disc_check.exe [WORKDIR]
Builds small ISO9660 images and folders with fake executables, runs disc_check on them, checks exit status and last line."""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

SECTOR = 2048


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def record(name, lba, size, is_dir=False):
    n = name.encode("ascii")
    rec_len = 33 + len(n)
    if rec_len & 1:
        rec_len += 1
    r = bytes([rec_len, 0]) + both32(lba) + both32(size) + bytes(7)
    r += bytes([2 if is_dir else 0, 0, 0]) + struct.pack("<H", 1) + struct.pack(">H", 1) + bytes([len(n)]) + n
    return r + bytes(rec_len - len(r))


def make_iso(path, files, pad_entries=0, truncate_at=None, keep_version=True):
    """files: {name: bytes}. Root directory at sector 20. pad_entries extra names push the entries over a sector boundary."""
    names = ["PAD%05d.BIN" % i for i in range(pad_entries)] + list(files)
    n_dir_sectors = 1 + (len(names) * 48) // SECTOR
    lba = 20 + n_dir_sectors + 1
    placed = []
    for nm in names:
        data = files.get(nm, b"")
        placed.append((nm, lba, data))
        lba += max(1, (len(data) + SECTOR - 1) // SECTOR)
    root = bytearray()
    entries = [record("\x00", 20, n_dir_sectors * SECTOR, True), record("\x01", 20, n_dir_sectors * SECTOR, True)]
    for nm, l, data in placed:
        entries.append(record(nm + (";1" if keep_version else ""), l, len(data)))
    for e in entries:                                  # an entry never crosses a sector: pad with zeros
        room = SECTOR - (len(root) % SECTOR)
        if len(e) > room:
            root += bytes(room)
        root += e
    root += bytes(n_dir_sectors * SECTOR - len(root))
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = b"PLAYSTATION".ljust(32)
    pvd[156:156 + 34] = record("\x00", 20, n_dir_sectors * SECTOR, True).ljust(34, b"\0")[:34]
    with open(path, "wb") as f:
        f.write(bytes(16 * SECTOR))
        f.write(pvd)
        f.write(bytes(3 * SECTOR))
        f.write(root)
        f.seek(lba * SECTOR)
        f.truncate()
        for nm, l, data in placed:
            f.seek(l * SECTOR)
            f.write(data)
        f.seek(lba * SECTOR)
        f.truncate()
        if truncate_at is not None:
            f.truncate(truncate_at)


def cnf(serial):
    return ("BOOT2 = cdrom0:\\%s;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n" % serial).encode()


def main():
    exe = os.path.abspath(sys.argv[1])
    work = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="disc_check_test_")
    os.makedirs(work, exist_ok=True)
    fake = bytes(range(256)) * 1000                    # not the game
    fake_sha = hashlib.sha256(fake).hexdigest()
    fails = 0

    def run(title, args, want_rc, want_last):
        nonlocal fails
        p = subprocess.run([exe] + args, capture_output=True)
        out = p.stdout.decode("utf-8", "replace").strip().splitlines()
        last = out[-1] if out else ""
        ok = p.returncode == want_rc and last.startswith(want_last)
        print("%-4s %-44s rc=%d  %s" % ("ok" if ok else "FAIL", title, p.returncode, last))
        if out and len(out) > 1:
            print("       " + out[0][:170])
        if not ok:
            fails += 1

    good_name = "SLUS_213.46"
    a = os.path.join(work, "wrong_exe.iso")
    make_iso(a, {"SYSTEM.CNF": cnf(good_name), good_name: fake})
    run("the right serial, another executable", [a], 1, "result: wrong (hash " + fake_sha)

    b = os.path.join(work, "pal.iso")
    make_iso(b, {"SYSTEM.CNF": cnf("SLES_999.99"), "SLES_999.99": fake})
    run("a European serial", [b], 1, "result: wrong (serial SLES_999.99)")

    c = os.path.join(work, "japan.iso")
    make_iso(c, {"SYSTEM.CNF": cnf("SLPS_999.99"), "SLPS_999.99": fake})
    run("a Japanese serial", [c], 1, "result: wrong (serial SLPS_999.99)")

    d = os.path.join(work, "no_cnf.iso")
    make_iso(d, {"README.TXT": b"hello"})
    run("an ISO with no SYSTEM.CNF (a data disc)", [d], 1, "result: wrong (not_ps2)")

    e = os.path.join(work, "not_an_iso.bin")
    with open(e, "wb") as f:
        f.write(os.urandom(200000))
    run("a file that is no ISO", [e], 2, "result: none (not_iso)")

    e2 = os.path.join(work, "tiny.bin")
    with open(e2, "wb") as f:
        f.write(b"x")
    run("a one-byte file", [e2], 2, "result: none (not_iso)")

    run("a path that does not exist", [os.path.join(work, "nothing.iso")], 2, "result: none (bad_path)")

    g = os.path.join(work, "cut.iso")
    make_iso(g, {"SYSTEM.CNF": cnf(good_name), good_name: fake}, truncate_at=30 * SECTOR)
    run("an image cut short", [g], 1, "result: wrong (no_executable)")

    h = os.path.join(work, "long_dir.iso")
    make_iso(h, {"SYSTEM.CNF": cnf(good_name), good_name: fake}, pad_entries=200)
    run("a root directory over several sectors", [h], 1, "result: wrong (hash " + fake_sha)

    i = os.path.join(work, "no_version.iso")
    make_iso(i, {"SYSTEM.CNF": cnf(good_name), good_name: fake}, keep_version=False)
    run("names without ;1", [i], 1, "result: wrong (hash " + fake_sha)

    folder = os.path.join(work, "folder")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "SYSTEM.CNF"), "wb") as f:
        f.write(cnf(good_name))
    with open(os.path.join(folder, good_name), "wb") as f:
        f.write(fake)
    run("a folder, another executable", [folder], 1, "result: wrong (hash " + fake_sha)
    run("a folder with a trailing slash", [folder + "\\"], 1, "result: wrong (hash " + fake_sha)

    empty = os.path.join(work, "empty_folder")
    os.makedirs(empty, exist_ok=True)
    run("an empty folder", [empty], 1, "result: wrong (not_ps2)")

    cyr_dir = os.path.join(work, "игры")
    os.makedirs(cyr_dir, exist_ok=True)
    cyr = os.path.join(cyr_dir, "диск.iso")
    shutil.copy(a, cyr)
    run("a Cyrillic path", [cyr], 1, "result: wrong (hash " + fake_sha)

    out = os.path.join(work, "extracted.bin")
    run("--extract with the wrong executable", ["--extract", out, a], 1, "result: wrong (hash")
    if os.path.exists(out):
        print("FAIL --extract left a file after a wrong hash")
        fails += 1

    run("no argument: looks at the disc drives", [], 2, "result: none (")
    run("an unknown option", ["--nonsense"], 2, "result: none (usage)")

    print("self-test: %s" % ("all passed" if not fails else "%d FAILED" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
