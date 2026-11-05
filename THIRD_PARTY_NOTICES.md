# Third-party notices

Our own code is under the Apache License 2.0 (`LICENSE`, `NOTICE`). Nothing below forbids that: every library the program links is
under a permissive licence, the GCC runtime carries its linking exception, and the GPL material (the cockpit models, xBRZ) is
not linked into the program and not in this source tree.

## In this repository

| Component | Where | Version | Licence | Text |
|---|---|---|---|---|
| ps2recomp: Ace Combat 5, by sal063 (recompiler, runtime, mod layer; our base) | `tools/ps2recomp`, `tools/vurecomp`, `runtime/` | fork of 2026-10-18 | Apache-2.0 | `LICENSE`; the changes are ours (`NOTICE`) |
| Dear ImGui, Omar Cornut (settings window) | `third_party/imgui` | 1.92.9b | MIT | `third_party/imgui/LICENSE.txt` |
| stb_truetype, stb_rectpack, stb_textedit, Sean Barrett (through ImGui; the quit panel's text) | `third_party/imgui/imstb_*.h` | as in ImGui | MIT or public domain, at your choice | end of each file |
| ProggyClean and ProggyForever fonts, Tristan Grimmer (embedded in ImGui as its default fonts) | `third_party/imgui/imgui_draw.cpp` | as in ImGui | MIT | ImGui's `docs/FONTS.md` upstream |
| Lua, PUC-Rio (mod scripts) | `third_party/lua` | 5.4.9 | MIT | `third_party/lua/LICENSE.html` |

The upstream licences' copyright lines stay in those files. Removing them is not allowed by their terms.

## Linked at build time, not in this repository

MSYS2 UCRT64 packages, versions as tested. A binary release has to carry these licence files next to the DLLs: MSYS2 installs them under
`ucrt64/share/licenses/<package>/`.

| Component | Used as | Tested version | Licence |
|---|---|---|---|
| SDL3 | `SDL3.dll`: window, input, audio | 3.4.18 | Zlib |
| OpenXR loader (Khronos OpenXR SDK) | `libopenxr_loader.dll`, headers | 1.1.62 | Apache-2.0 |
| Vulkan headers and loader (Khronos) | import library; `vulkan-1.dll` comes with the GPU driver | 1.4.363 | Apache-2.0 |
| GCC runtime: libgcc, libstdc++ | static in `ac5.exe`; `libgcc_s_seh-1.dll`, `libstdc++-6.dll` for the OpenXR loader | GCC 16.2 (15 or newer needed) | GPL-3.0-or-later with the GCC Runtime Library Exception 3.1, which lets our program carry them under its own licence |
| winpthreads (mingw-w64) | `libwinpthread-1.dll` | 14.0.0 | MIT and BSD-3-Clause |
| mingw-w64 CRT startup code | in `ac5.exe` | 14.0.0 | Zope Public License 2.1, public domain parts |

Build tools only, nothing of them ends up in the program: GCC (GPL-3.0-or-later), CMake (BSD-3-Clause), Ninja (Apache-2.0), glslc / shaderc (Apache-2.0; the
compiled SPIR-V is ours), Python (PSF).

## Optional tools the scripts can use

None of them is in this repository; each is fetched or installed by whoever wants it.

| Tool | Used by | Licence | Note |
|---|---|---|---|
| xBRZ, Zenju | `tools/textures/build_xbrz.sh` downloads and builds it for `upscale_hud.py` | GPL-3.0 | `tools/textures/xbrz_cli.cpp` is our small wrapper under Apache-2.0; a binary built from it with xBRZ is under the GPL-3.0 |
| Real-ESRGAN, Xintao Wang et al. (the models) and realesrgan-ncnn-vulkan | `tools/cockpit/upscale_tex.sh` | BSD-3-Clause; MIT | run as a separate program |
| FFmpeg | the optional upscaled movies (`ACZ_FMV_FFMPEG`) | LGPL or GPL, by build | run as a separate program, not linked |
| numpy, Pillow | a few cockpit paint scripts (`PYV`) | BSD-3-Clause, MIT-CMU | |

## Ideas and references, no code copied

The cloud noise follows the construction of sebh/TileableVolumeNoise (Sebastien Hillaire, MIT), written anew in GLSL
(`runtime/shaders/cloud_noise.comp`). The cloud rendering takes ideas from the published Nubis (Guerrilla) and Frostbite (Hillaire) talks.

## Texture packs

`textures/xbox/` and `textures/xbox_modern/` are our own art, under the Apache-2.0 like the code: button glyphs drawn by
`tools/textures/make_glyphs.py`, with letter shapes rasterised from the Segoe UI Bold on the machine that ran it. They are named
after the runtime content hash of the game texture they stand in for; a hash is not a picture. The button names and colours
only say which controller button is meant; Xbox is a trademark of Microsoft.

The upscaled HUD and menu packs are not in this repository, because they are derivatives of the game's own pictures. The tools
that make them from your own disc are: `tools/textures/make_hud_pack.py` and `make_pack_from_dump.py`, from a run with `PS2_TEX_DUMP`.
Without them the game shows its original HUD and menu textures. What you make from your disc is for your own use.

## Cockpit models

No cockpit model is in this repository. The public cockpit set is a separate download with its own `LICENSES.md`: per model the
source, the authors, the licence (GPL-2.0, GPL-3.0 and CC0 models) and the source offer. A set that contains the GPL-3.0 models is
GPL-3.0 as a whole. The program reads those files as data and is not a derivative of them; the converter in `tools/cockpit` is
ours. Texture upscales in the set are derivative works of the GPL textures, under the same licence.
