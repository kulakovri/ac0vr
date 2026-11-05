# The CI's tiny generated set

The runtime only links against a generated set: `ps2_code_*.c`, `ps2_func_table.c`, `ps2_funcs.h`, `ps2_image.c`, `ps2_symbols.c`
and, at run time, `ps2_image.bin`. A real set is made from a player's disc and may never enter this repository, so GitHub
Actions compiles the runtime against `ci/tiny_gen/` instead (`.github/workflows/build.yml`).

**What it is.** The recompiler's output for a 22-instruction PS2 EE program of our own: a stack frame, a counted loop around a
call, 64-bit addition, a 128-bit load and store, an indirect call and a syscall. `ci/make_tiny_elf.py` encodes the instructions
by hand and writes the ELF (about 4 KB), so no PS2 toolchain and no SDK are involved. Two functions come out; the third is reached
only through a computed pointer, as in real games, and stays to the dispatcher. No game bytes, no SDK symbols, no overrides.

**How it is made.**

```
python ci/make_tiny_elf.py build_ci_tiny.elf
PYTHONPATH=tools python -m ps2recomp build_ci_tiny.elf -o ci/tiny_gen
```

The CI repeats this into a scratch folder and fails when the result differs from `ci/tiny_gen/`, so a recompiler change that alters
the output has to come with a refreshed set.

**What it proves.** The runtime, the shaders and the recompiler's output compile and link on a clean Windows runner with MSYS2 UCRT64.
It does not run anything: the program is never executed, the runner has no GPU, no headset and no game.
