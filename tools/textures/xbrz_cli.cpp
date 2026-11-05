// xbrz_cli SCALE IN.raw OUT.raw [argb|rgb]: scale an RGBA image with xBRZ (Zenju, GPL-3.0, https://sourceforge.net/
// projects/xbrz; not in this repo: build_xbrz.sh downloads it). Raw files: u32 width, u32 height (little endian),
// then width*height RGBA bytes. Used by upscale_hud.py.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include "xbrz.h"

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: xbrz_cli SCALE IN.raw OUT.raw [argb|rgb]\n");
        return 2;
    }
    int sc = std::atoi(argv[1]);
    bool rgb = argc > 4 && !std::strcmp(argv[4], "rgb");
    FILE *f = std::fopen(argv[2], "rb");
    if (!f) { std::perror(argv[2]); return 1; }
    uint32_t wh[2];
    if (std::fread(wh, 4, 2, f) != 2 || !wh[0] || !wh[1] || wh[0] > 8192 || wh[1] > 8192) { std::fclose(f); return 1; }
    std::vector<uint8_t> in((size_t)wh[0] * wh[1] * 4);
    if (std::fread(in.data(), 1, in.size(), f) != in.size()) { std::fclose(f); return 1; }
    std::fclose(f);
    std::vector<uint32_t> src((size_t)wh[0] * wh[1]), dst((size_t)wh[0] * wh[1] * sc * sc);
    for (size_t i = 0; i < src.size(); i++)
        src[i] = ((uint32_t)in[4 * i + 3] << 24) | ((uint32_t)in[4 * i] << 16) | ((uint32_t)in[4 * i + 1] << 8) | in[4 * i + 2];
    xbrz::scale((size_t)sc, src.data(), dst.data(), (int)wh[0], (int)wh[1],
                rgb ? xbrz::ColorFormat::rgb : xbrz::ColorFormat::argb);
    std::vector<uint8_t> out(dst.size() * 4);
    for (size_t i = 0; i < dst.size(); i++) {
        out[4 * i] = (uint8_t)(dst[i] >> 16); out[4 * i + 1] = (uint8_t)(dst[i] >> 8);
        out[4 * i + 2] = (uint8_t)dst[i]; out[4 * i + 3] = rgb ? 255 : (uint8_t)(dst[i] >> 24);
    }
    f = std::fopen(argv[3], "wb");
    if (!f) { std::perror(argv[3]); return 1; }
    uint32_t owh[2] = { wh[0] * (uint32_t)sc, wh[1] * (uint32_t)sc };
    std::fwrite(owh, 4, 2, f);
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return 0;
}
