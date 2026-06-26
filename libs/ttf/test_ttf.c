// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host test for libs/ttf (stb_truetype wrapper). The rasterizer is float-based and
// runs natively here, so we can check it produces sane glyph coverage from the real
// vendored font -- the part the kernel runs but QEMU can't assert deterministically.
//
//   clang -I../stb ttf.c test_ttf.c -o /tmp/test_ttf && /tmp/test_ttf <font.ttf>
// (the build harness passes lisp/data/DejaVuSans-subset.ttf)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ttf.h"

static int failures = 0;
static void check(int cond, const char *msg) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", msg);
    if (!cond) failures++;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "../../lisp/data/DejaVuSans-subset.ttf";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open font %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *font = malloc((size_t)n);
    if (fread(font, 1, (size_t)n, f) != (size_t)n) { printf("FAIL: read\n"); return 1; }
    fclose(f);

    printf("[ttf] stb_truetype glyph rasterization (%ld-byte font)\n", n);

    int w, h, xo, yo, adv;
    // 'A' at 32px: a real glyph -- non-empty coverage, plausible dimensions + advance.
    uint8_t *a = ttf_rasterize(font, (size_t)n, 'A', 32, &w, &h, &xo, &yo, &adv);
    check(a != NULL, "'A' rasterized to a bitmap");
    check(w > 5 && w < 40 && h > 10 && h < 40, "'A' has plausible WxH");
    check(adv > 5 && adv < 40, "'A' has a plausible advance");
    int nz = 0, full = 0;
    if (a) for (int i = 0; i < w * h; i++) { if (a[i]) nz++; if (a[i] == 255) full++; }
    check(nz > 0, "'A' coverage has set pixels");
    check(full > 0 && nz < w * h, "'A' is partially covered (real antialiased glyph)");
    // antialiasing: some pixels are partial (not just 0/255).
    int partial = 0;
    if (a) for (int i = 0; i < w * h; i++) if (a[i] > 0 && a[i] < 255) partial++;
    check(partial > 0, "'A' has partial-coverage (antialiased) edge pixels");
    ttf_free_bitmap(a);

    // space: empty glyph, NULL bitmap, but a positive advance.
    int sw, sh;
    uint8_t *sp = ttf_rasterize(font, (size_t)n, ' ', 32, &sw, &sh, &xo, &yo, &adv);
    check(sp == NULL, "space rasterizes to no bitmap");
    check(adv > 0, "space still has a positive advance");
    ttf_free_bitmap(sp);

    // a wider glyph advances more than a narrow one.
    int aw, mw;
    uint8_t *gi = ttf_rasterize(font, (size_t)n, 'i', 32, &w, &h, &xo, &yo, &aw);
    uint8_t *gm = ttf_rasterize(font, (size_t)n, 'M', 32, &w, &h, &xo, &yo, &mw);
    check(mw > aw, "'M' advances wider than 'i' (proportional metrics)");
    ttf_free_bitmap(gi); ttf_free_bitmap(gm);

    // scaling: 2x pixel height ~ 2x glyph height.
    int h1, h2;
    uint8_t *g1 = ttf_rasterize(font, (size_t)n, 'H', 20, &w, &h1, &xo, &yo, &adv);
    uint8_t *g2 = ttf_rasterize(font, (size_t)n, 'H', 40, &w, &h2, &xo, &yo, &adv);
    check(h2 > h1 * 3 / 2, "'H' at 40px is taller than at 20px");
    ttf_free_bitmap(g1); ttf_free_bitmap(g2);

    // vmetrics: ascent > 0, descent < 0, sane line pitch.
    int asc, desc, lg;
    ttf_vmetrics(font, (size_t)n, 32, &asc, &desc, &lg);
    check(asc > 0 && desc < 0 && (asc - desc) > 20 && (asc - desc) < 60, "vmetrics plausible");
    printf("       ascent=%d descent=%d linegap=%d\n", asc, desc, lg);

    printf("\n[ttf] %s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
