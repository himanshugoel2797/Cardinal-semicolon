/**
 * Copyright (c) 2026 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

// Self-contained PNG decoder.
//
// The original Cardinal "ihd" boot path used a libpng wrapper; this reuses the
// miniz inflate already bundled in the tree (libs/miniz) for the DEFLATE step
// and implements PNG chunk parsing + scanline unfiltering directly, so no
// libpng/zlib dependency is needed.
//
// Scope (v1): 8-bit, non-interlaced, color type 2 (RGB) or 6 (RGBA). Other
// formats return NULL. Output is BGRA8888 (framebuffer-ready).

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

#include "png.h"

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// PNG Paeth predictor (RFC 2083 section 6.6).
static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc)
        return a;
    if (pb <= pc)
        return b;
    return c;
}

void *DecodePNGtoRGBA(const void *src, int len, int *img_w, int *img_h, int *img_p, int *res_len)
{
    const uint8_t *data = (const uint8_t *)src;
    if (data == NULL || len < 8)
        return NULL;

    static const uint8_t magic[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    for (int i = 0; i < 8; i++)
        if (data[i] != magic[i])
            return NULL;

    uint32_t width = 0, height = 0;
    int bit_depth = 0, color_type = -1, interlace = 0;
    int seen_ihdr = 0;
    size_t idat_total = 0;

    // First pass: read IHDR and total up the IDAT payload length.
    for (size_t pos = 8; pos + 12 <= (size_t)len;)
    {
        uint32_t clen = be32(data + pos);
        const uint8_t *ctype = data + pos + 4;
        size_t cstart = pos + 8;
        if (cstart + (size_t)clen + 4 > (size_t)len) // data + 4-byte CRC must fit
            break;

        if (memcmp(ctype, "IHDR", 4) == 0)
        {
            if (clen < 13)
                return NULL;
            width = be32(data + cstart);
            height = be32(data + cstart + 4);
            bit_depth = data[cstart + 8];
            color_type = data[cstart + 9];
            interlace = data[cstart + 12];
            seen_ihdr = 1;
        }
        else if (memcmp(ctype, "IDAT", 4) == 0)
        {
            idat_total += clen;
        }
        else if (memcmp(ctype, "IEND", 4) == 0)
        {
            break;
        }

        pos = cstart + (size_t)clen + 4;
    }

    if (!seen_ihdr || width == 0 || height == 0 || idat_total == 0)
        return NULL;

    // v1 supported formats only.
    if (bit_depth != 8 || interlace != 0)
        return NULL;

    int channels;
    if (color_type == 2)
        channels = 3; // truecolor RGB
    else if (color_type == 6)
        channels = 4; // truecolor + alpha
    else
        return NULL;

    // Bound the geometry so the size math can't overflow.
    if (width > 0x7FFF || height > 0x7FFF)
        return NULL;

    // Second pass: gather the IDAT chunks into one contiguous zlib stream.
    uint8_t *idat = (uint8_t *)malloc(idat_total);
    if (idat == NULL)
        return NULL;

    size_t idat_off = 0;
    for (size_t pos = 8; pos + 12 <= (size_t)len;)
    {
        uint32_t clen = be32(data + pos);
        const uint8_t *ctype = data + pos + 4;
        size_t cstart = pos + 8;
        if (cstart + (size_t)clen + 4 > (size_t)len)
            break;

        if (memcmp(ctype, "IDAT", 4) == 0)
        {
            memcpy(idat + idat_off, data + cstart, clen);
            idat_off += clen;
        }
        else if (memcmp(ctype, "IEND", 4) == 0)
        {
            break;
        }

        pos = cstart + (size_t)clen + 4;
    }

    // Inflate into raw scanlines: each row is one filter byte + width*channels.
    size_t row_bytes = (size_t)width * channels;
    size_t raw_size = (row_bytes + 1) * height;
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    if (raw == NULL)
    {
        free(idat);
        return NULL;
    }

    size_t out = tinfl_decompress_mem_to_mem(
        raw, raw_size, idat, idat_total,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(idat);
    if (out != raw_size)
    {
        free(raw);
        return NULL;
    }

    uint8_t *outbuf = (uint8_t *)malloc((size_t)width * height * 4);
    if (outbuf == NULL)
    {
        free(raw);
        return NULL;
    }

    // Reverse the per-scanline filters in place, then emit BGRA. Each filtered
    // byte references the already-reconstructed pixel to the left (a), above
    // (b) and above-left (c).
    int bpp = channels;
    for (uint32_t y = 0; y < height; y++)
    {
        uint8_t *row = raw + (size_t)y * (row_bytes + 1);
        uint8_t filter = row[0];
        uint8_t *cur = row + 1;
        uint8_t *prev = (y == 0) ? NULL : (raw + (size_t)(y - 1) * (row_bytes + 1) + 1);

        for (size_t x = 0; x < row_bytes; x++)
        {
            int a = (x >= (size_t)bpp) ? cur[x - bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= (size_t)bpp) ? prev[x - bpp] : 0;
            int v = cur[x];
            switch (filter)
            {
            case 0:
                break; // None
            case 1:
                v += a; // Sub
                break;
            case 2:
                v += b; // Up
                break;
            case 3:
                v += (a + b) / 2; // Average
                break;
            case 4:
                v += paeth(a, b, c); // Paeth
                break;
            default:
                free(raw);
                free(outbuf);
                return NULL;
            }
            cur[x] = (uint8_t)v;
        }

        for (uint32_t x = 0; x < width; x++)
        {
            uint8_t r = cur[x * channels + 0];
            uint8_t g = cur[x * channels + 1];
            uint8_t bl = cur[x * channels + 2];
            uint8_t al = (channels == 4) ? cur[x * channels + 3] : 255;
            uint8_t *px = outbuf + ((size_t)y * width + x) * 4;
            px[0] = bl; // B
            px[1] = g;  // G
            px[2] = r;  // R
            px[3] = al; // A
        }
    }

    free(raw);

    if (img_w != NULL)
        *img_w = (int)width;
    if (img_h != NULL)
        *img_h = (int)height;
    if (img_p != NULL)
        *img_p = (int)(width * 4);
    if (res_len != NULL)
        *res_len = (int)(width * height * 4);

    return outbuf;
}
