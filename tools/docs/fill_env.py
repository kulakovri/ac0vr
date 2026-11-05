"""Fill the @@VAR@@ / @@LINE:file:text@@ placeholders of env_template.md with fresh source locations.

usage: python -I fill_env.py <repo root> <template> <output>
Prints unresolved placeholders, variables read in the source but missing from the template, and row counts per section.
"""
import os
import re
import sys
import datetime

root, template, output = sys.argv[1], sys.argv[2], sys.argv[3]

READ = re.compile(r'\b(getenv|ps2_menv|ps2_menv_boot|env_float|envf|cap_env_int|env_note|env_flag|vk_env_flag|PS2_ENV|mfd_env_list|env_int|env_f|env_i|sp_env|env_or|hgov_envf)\s*\(\s*"([A-Za-z0-9_]+)"')
sites = {}
src_dirs = [os.path.join(root, "runtime", "src"), os.path.join(root, "runtime", "include")]
for d in src_dirs:
    for dp, _, files in os.walk(d):
        for fn in sorted(files):
            if not fn.endswith((".c", ".cpp", ".h", ".inc")):
                continue
            path = os.path.join(dp, fn)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            with open(path, encoding="utf-8", errors="replace") as f:
                for no, line in enumerate(f, 1):
                    for m in READ.finditer(line):
                        name = m.group(2)
                        loc = "%s:%d" % (rel, no)
                        lst = sites.setdefault(name, [])
                        if loc not in lst:
                            lst.append(loc)


def fmt(locs, cap=3):
    shown = ", ".join("`%s`" % l for l in locs[:cap])
    if len(locs) > cap:
        shown += " (+%d more)" % (len(locs) - cap)
    return shown


def line_of(relfile, text):
    path = os.path.join(root, relfile)
    with open(path, encoding="utf-8", errors="replace") as f:
        for no, line in enumerate(f, 1):
            if text in line:
                return "`%s:%d`" % (relfile, no)
    return None


with open(template, encoding="utf-8") as f:
    doc = f.read()

doc = doc.replace("@@DATE@@", datetime.date.today().isoformat())
unresolved = []
used = set()


def repl(m):
    key = m.group(1)
    if key.startswith("LINE:"):
        _, relfile, text = key.split(":", 2)
        r = line_of(relfile, text)
        if r is None:
            unresolved.append(key)
            return m.group(0)
        return r
    used.add(key)
    if key not in sites:
        unresolved.append(key)
        return m.group(0)
    return fmt(sites[key])


doc = re.sub(r"@@(.+?)@@", repl, doc)

with open(output, "w", encoding="utf-8", newline="\n") as f:
    f.write(doc)

print("unresolved:", unresolved)
missing = sorted(set(sites) - used)
print("read in source but not documented:", missing)

# rows per top-level section
counts = []
cur = None
in_var_table = False
for line in doc.splitlines():
    if line.startswith("## "):
        cur = line[3:].strip()
        counts.append([cur, 0])
        in_var_table = False
    elif line.startswith("| Variable | Default"):
        in_var_table = True
    elif not line.startswith("|"):
        in_var_table = False
    elif in_var_table and line.startswith("| `") and counts:
        counts[-1][1] += 1
for name, n in counts:
    print("%-40s %d" % (name, n))
print("variables found in source:", len(sites))
