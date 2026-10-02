// Host-test stand-in for <gx2/enum.h>: just the values the plugin sources name.
#pragma once
#include "../wut_types.h"

typedef enum GX2SurfaceFormat {
    GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8 = 0x01a,
    GX2_SURFACE_FORMAT_SRGB_R8_G8_B8_A8  = 0x41a,
} GX2SurfaceFormat;

typedef enum GX2ScanTarget {
    GX2_SCAN_TARGET_TV  = 1,
    GX2_SCAN_TARGET_DRC = 4,
} GX2ScanTarget;
