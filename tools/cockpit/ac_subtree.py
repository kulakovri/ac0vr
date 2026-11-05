"""ac_subtree.py IN.ac OUT.ac --drop NAME [--drop NAME ...] -- copy an AC3D file without the objects (and everything
below them) whose name is NAME, fixing the parents' kids counts. Used to take a FlightGear pilot model's ejection seat
without its pilot (build_a10a.sh: the A-10's Models/pilot.ac holds the groups "seat" and "Pilot"). Only reads IN.ac."""
import sys


def main():
    a = sys.argv[1:]
    if len(a) < 4 or '--drop' not in a:
        sys.exit(__doc__)
    src, dst = a[0], a[1]
    drop = {a[i + 1] for i, x in enumerate(a) if x == '--drop' and i + 1 < len(a)}
    text = open(src, 'r', encoding='latin-1', newline='').read()
    lines = text.splitlines(keepends=True)
    pos = 0

    def skip_data(i, n):                     # a "data N" block: N characters on the following line(s)
        got = 0
        while got < n and i < len(lines):
            got += len(lines[i].rstrip('\r\n')) + (1 if got else 0)
            i += 1
        return i

    def parse(i):
        """object starting at line i ("OBJECT ..."): returns (end line, name, kids line index, children spans)"""
        name, kids_at = None, None
        i += 1
        while i < len(lines):
            w = lines[i].split()
            if not w:
                i += 1; continue
            k = w[0]
            if k == 'name':
                name = lines[i].strip()[5:].strip().strip('"'); i += 1
            elif k == 'data':
                i = skip_data(i + 1, int(w[1]))
            elif k == 'numvert':
                i += 1 + int(w[1])
            elif k == 'numsurf':
                i += 1
                for _ in range(int(w[1])):
                    while i < len(lines) and not lines[i].startswith('refs'):
                        i += 1
                    i += 1 + int(lines[i].split()[1])
            elif k == 'kids':
                kids_at = i
                n = int(w[1]); i += 1
                ch = []
                for _ in range(n):
                    s = i
                    i, cn, cka, cch = parse(i)
                    ch.append((s, i, cn, cka, cch))
                return i, name, kids_at, ch
            else:
                i += 1
        return i, name, kids_at, []

    while pos < len(lines) and not lines[pos].startswith('OBJECT'):
        pos += 1
    end, name, kids_at, ch = parse(pos)
    removed, out_skip, kid_fix = [], set(), {}

    def walk(kids_line, children):
        keep = 0
        for s, e, cn, cka, cch in children:
            if cn in drop:
                out_skip.update(range(s, e)); removed.append((cn, e - s))
            else:
                keep += 1
                walk(cka, cch)
        if kids_line is not None and keep != len(children):
            kid_fix[kids_line] = keep

    walk(kids_at, ch)
    with open(dst, 'w', encoding='latin-1', newline='') as f:
        for i, ln in enumerate(lines):
            if i in out_skip:
                continue
            if i in kid_fix:
                nl = ln[len(ln.rstrip('\r\n')):]
                ln = 'kids %d%s' % (kid_fix[i], nl)
            f.write(ln)
    print('%s: dropped %s' % (dst, ', '.join('%s (%d lines)' % r for r in removed) or 'nothing'))


if __name__ == '__main__':
    main()
