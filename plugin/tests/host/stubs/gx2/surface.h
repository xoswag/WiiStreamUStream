// Host-test stand-in for <gx2/surface.h>, field names as in wut.
#pragma once
#include "enum.h"

typedef struct GX2Surface {
    uint32_t dim;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mipLevels;
    GX2SurfaceFormat format;
    uint32_t aa;
    uint32_t use;
    uint32_t imageSize;
    void *image;
    uint32_t mipmapSize;
    void *mipmaps;
    uint32_t tileMode;
    uint32_t swizzle;
    uint32_t alignment;
    uint32_t pitch;
    uint32_t mipLevelOffset[13];
} GX2Surface;

typedef struct GX2ColorBuffer {
    GX2Surface surface;
    uint32_t viewMip;
    uint32_t viewFirstSlice;
    uint32_t viewNumSlices;
    void *aaBuffer;
    uint32_t aaSize;
    uint32_t regs[5];
} GX2ColorBuffer;
