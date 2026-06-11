// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_PNG_H
#define CARDINALSEMI_PNG_H

// Decode an 8-bit, non-interlaced truecolor (RGB) or truecolor+alpha (RGBA)
// PNG into a freshly malloc'd BGRA8888 buffer. The byte order is B, G, R, A,
// which read as a little-endian uint32 is 0xAARRGGBB -- the XRGB8888 pixel
// format used by the display planes / linear framebuffer.
//
// Returns the pixel buffer (caller frees with free()) or NULL on error or
// unsupported format. On success the (optional) out-params receive the image
// width, height, pitch (bytes per row = width*4) and total byte length.
void *DecodePNGtoRGBA(const void *src, int len, int *img_w, int *img_h, int *img_p, int *res_len);

#endif
