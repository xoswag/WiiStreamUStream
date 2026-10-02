/****************************************************************************
 * Copyright (C) 2018 Maschell
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 ****************************************************************************/
#include "config.hpp"
#include "ImageEncoder.hpp"
#include "retain_vars.hpp"
#include "utils/logger.h"

#include <wups.h>
#include <wups/config/WUPSConfigItemIntegerRange.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config_api.h>
#include <wups/storage.h>

namespace StreamConfig {
namespace {

constexpr const char *KEY_SCREEN       = "screen";
constexpr const char *KEY_CAPTURE_SIZE = "captureSize";
constexpr const char *KEY_COLOR_MODE   = "colorMode";
constexpr const char *KEY_QUALITY      = "quality";
constexpr const char *KEY_FRAME_SKIP   = "frameSkip";
// A new key rather than reusing "encoderCore": the old setting stored a core
// number, and read as a preset index it would mean something different.
constexpr const char *KEY_ENCODER_CORES = "encoderCores";
constexpr const char *KEY_ENCODE_PATH   = "encodePath";
constexpr const char *KEY_GPU_SYNC      = "gpuSync";
constexpr const char *KEY_AUDIO         = "audio";

ConfigItemMultipleValuesPair sScreenValues[] = {
        {WUPS_STREAMING_SCREEN_TV, "TV"},
        {WUPS_STREAMING_SCREEN_DRC, "GamePad"},
};

ConfigItemMultipleValuesPair sCaptureSizeValues[] = {
        {WUPS_STREAMING_SIZE_NATIVE, "Native (720p on most games)"},
        {WUPS_STREAMING_SIZE_720P, "720p max"},
        {WUPS_STREAMING_SIZE_480P, "480p max"},
        {WUPS_STREAMING_SIZE_360P, "360p max"},
        {WUPS_STREAMING_SIZE_240P, "240p max"},
        {WUPS_STREAMING_SIZE_180P, "180p max"},
};

ConfigItemMultipleValuesPair sColorValues[] = {
        {WUPS_STREAMING_COLOR_AUTO, "Auto (sRGB correct)"},
        {WUPS_STREAMING_COLOR_RAW, "Raw"},
};

ConfigItemMultipleValuesPair sCoreValues[] = {
        {WUPS_STREAMING_CORES_0_2, "Cores 0 + 2"},
        {WUPS_STREAMING_CORES_0, "Core 0 only"},
        {WUPS_STREAMING_CORES_2, "Core 2 only"},
        {WUPS_STREAMING_CORES_1, "Core 1 only"},
        {WUPS_STREAMING_CORES_ALL, "All three (0 + 2 + 1)"},
};

ConfigItemMultipleValuesPair sPathValues[] = {
        {WUPS_STREAMING_PATH_FAST, "Fast (multi-core)"},
        {WUPS_STREAMING_PATH_SAFE, "Safe (old single-core)"},
};

ConfigItemMultipleValuesPair sGpuSyncValues[] = {
        {WUPS_STREAMING_GPUSYNC_ASYNC, "Async (game never waits)"},
        {WUPS_STREAMING_GPUSYNC_BLOCKING, "Blocking (old)"},
};

ConfigItemMultipleValuesPair sAudioValues[] = {
        {1, "On"},
        {0, "Off (from the next game start)"},
};

#define COUNT(arr) ((int) (sizeof(arr) / sizeof((arr)[0])))

/**
 * The API wants the *index* of the current entry, not the value, so a stored
 * setting has to be looked up rather than passed straight through.
 */
int indexOfValue(const ConfigItemMultipleValuesPair *pairs, int count, int32_t value) {
    for (int i = 0; i < count; i++) {
        if ((int32_t) pairs[i].value == value) {
            return i;
        }
    }
    return 0;
}

int32_t loadU32(const char *key, int32_t defaultValue, int32_t minValue, int32_t maxValue) {
    uint32_t stored = 0;
    if (WUPSStorageAPI_GetU32(nullptr, key, &stored) != WUPS_STORAGE_ERROR_SUCCESS) {
        WUPSStorageAPI_StoreU32(nullptr, key, (uint32_t) defaultValue);
        return defaultValue;
    }
    auto value = (int32_t) stored;
    if (value < minValue || value > maxValue) {
        return defaultValue;
    }
    return value;
}

void storeU32(const char *key, int32_t value) {
    if (WUPSStorageAPI_StoreU32(nullptr, key, (uint32_t) value) != WUPS_STORAGE_ERROR_SUCCESS) {
        DEBUG_FUNCTION_LINE_WARN("Failed to persist \"%s\"", key);
    }
}

void screenChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    gScreen = (int32_t) newValue;
    storeU32(KEY_SCREEN, gScreen);
}

void captureSizeChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    gCaptureSize = (int32_t) newValue;
    storeU32(KEY_CAPTURE_SIZE, gCaptureSize);
}

void colorModeChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    gColorMode = (int32_t) newValue;
    storeU32(KEY_COLOR_MODE, gColorMode);
}

void qualityChanged(ConfigItemIntegerRange *, int32_t newValue) {
    gQuality = newValue;
    storeU32(KEY_QUALITY, gQuality);
}

void frameSkipChanged(ConfigItemIntegerRange *, int32_t newValue) {
    gFrameSkip = newValue;
    storeU32(KEY_FRAME_SKIP, gFrameSkip);
}

void encoderCoresChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    const int32_t preset = (int32_t) newValue;
    if (preset == gEncoderCores) {
        return;
    }
    gEncoderCores = preset;
    storeU32(KEY_ENCODER_CORES, gEncoderCores);

    // Affinity is fixed when a thread is created, so this is the one setting
    // that needs the encoder rebuilt. Everything else is read per frame.
    if (ImageEncoder::IsRunning()) {
        ImageEncoder::Stop();
        ImageEncoder::Start();
    }
}

void encodePathChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    gEncodePath = (int32_t) newValue;
    storeU32(KEY_ENCODE_PATH, gEncodePath);
}

void gpuSyncChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    gGpuSync = (int32_t) newValue;
    storeU32(KEY_GPU_SYNC, gGpuSync);
    if (gGpuSync == WUPS_STREAMING_GPUSYNC_ASYNC) {
        // Choosing Async explicitly gives it a fresh chance in this title, even if
        // the encoder had fallen back to blocking.
        gGpuSyncFallback = false;
    }
}

void audioChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    // Takes effect at once for a title whose mixer is already hooked (capture just
    // stops or resumes). Hooking itself happens when a title starts its audio, so
    // switching Off -> On mid-title applies from the next title, and Off at title
    // start means nothing of ours is put into the mixer at all.
    gAudioEnabled = (int32_t) newValue;
    storeU32(KEY_AUDIO, gAudioEnabled);
}

WUPSConfigAPICallbackStatus menuOpened(WUPSConfigCategoryHandle rootHandle) {
    if (WUPSConfigItemMultipleValues_AddToCategory(
                rootHandle, KEY_SCREEN, "Screen to stream",
                indexOfValue(sScreenValues, 2, WUPS_STREAMING_SCREEN_TV),
                indexOfValue(sScreenValues, 2, gScreen),
                sScreenValues, 2, &screenChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(
                rootHandle, KEY_CAPTURE_SIZE, "Resolution",
                indexOfValue(sCaptureSizeValues, COUNT(sCaptureSizeValues), WUPS_STREAMING_SIZE_NATIVE),
                indexOfValue(sCaptureSizeValues, COUNT(sCaptureSizeValues), gCaptureSize),
                sCaptureSizeValues, COUNT(sCaptureSizeValues), &captureSizeChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemIntegerRange_AddToCategory(
                rootHandle, KEY_QUALITY, "JPEG quality",
                55, gQuality, STREAM_QUALITY_MIN, STREAM_QUALITY_MAX,
                &qualityChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemIntegerRange_AddToCategory(
                rootHandle, KEY_FRAME_SKIP, "Skip N frames between captures",
                0, gFrameSkip, STREAM_FRAMESKIP_MIN, STREAM_FRAMESKIP_MAX,
                &frameSkipChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(
                rootHandle, KEY_COLOR_MODE, "Colour",
                indexOfValue(sColorValues, 2, WUPS_STREAMING_COLOR_AUTO),
                indexOfValue(sColorValues, 2, gColorMode),
                sColorValues, 2, &colorModeChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(
                rootHandle, KEY_ENCODER_CORES, "Encoder cores",
                indexOfValue(sCoreValues, COUNT(sCoreValues), WUPS_STREAMING_CORES_0_2),
                indexOfValue(sCoreValues, COUNT(sCoreValues), gEncoderCores),
                sCoreValues, COUNT(sCoreValues), &encoderCoresChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(
                rootHandle, KEY_ENCODE_PATH, "Encoder",
                indexOfValue(sPathValues, COUNT(sPathValues), WUPS_STREAMING_PATH_FAST),
                indexOfValue(sPathValues, COUNT(sPathValues), gEncodePath),
                sPathValues, COUNT(sPathValues), &encodePathChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(
                rootHandle, KEY_GPU_SYNC, "GPU sync",
                indexOfValue(sGpuSyncValues, COUNT(sGpuSyncValues), WUPS_STREAMING_GPUSYNC_ASYNC),
                indexOfValue(sGpuSyncValues, COUNT(sGpuSyncValues), gGpuSync),
                sGpuSyncValues, COUNT(sGpuSyncValues), &gpuSyncChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(
                rootHandle, KEY_AUDIO, "Audio",
                indexOfValue(sAudioValues, COUNT(sAudioValues), 1),
                indexOfValue(sAudioValues, COUNT(sAudioValues), gAudioEnabled),
                sAudioValues, COUNT(sAudioValues), &audioChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void menuClosed() {
    // Item callbacks already wrote each value; this just flushes them to the SD card.
    if (WUPSStorageAPI_SaveStorage(false) != WUPS_STORAGE_ERROR_SUCCESS) {
        DEBUG_FUNCTION_LINE_WARN("Failed to save the settings");
    }
}

} // namespace

void Init() {
    gScreen      = loadU32(KEY_SCREEN, WUPS_STREAMING_SCREEN_TV, 0, 1);
    gCaptureSize = loadU32(KEY_CAPTURE_SIZE, WUPS_STREAMING_SIZE_NATIVE, 0, WUPS_STREAMING_SIZE_LAST);
    gColorMode   = loadU32(KEY_COLOR_MODE, WUPS_STREAMING_COLOR_AUTO, 0, 1);
    gQuality     = loadU32(KEY_QUALITY, 55, STREAM_QUALITY_MIN, STREAM_QUALITY_MAX);
    // Default 0: capturing is cheap for the game now (no GPU stall, no per-frame
    // cache flush) and only happens when the encoder has a slot free, so skipping
    // frames on purpose just caps a 60 fps game at 30.
    gFrameSkip    = loadU32(KEY_FRAME_SKIP, 0, STREAM_FRAMESKIP_MIN, STREAM_FRAMESKIP_MAX);
    gEncoderCores = loadU32(KEY_ENCODER_CORES, WUPS_STREAMING_CORES_0_2, 0, WUPS_STREAMING_CORES_LAST);
    gEncodePath   = loadU32(KEY_ENCODE_PATH, WUPS_STREAMING_PATH_FAST, WUPS_STREAMING_PATH_FAST, WUPS_STREAMING_PATH_SAFE);
    gGpuSync      = loadU32(KEY_GPU_SYNC, WUPS_STREAMING_GPUSYNC_ASYNC, WUPS_STREAMING_GPUSYNC_ASYNC, WUPS_STREAMING_GPUSYNC_BLOCKING);
    gAudioEnabled = loadU32(KEY_AUDIO, 1, 0, 1);

    WUPSConfigAPIOptionsV1 options = {.name = "Screen Streaming"};
    const WUPSConfigAPIStatus status = WUPSConfigAPI_Init(options, menuOpened, menuClosed);
    if (status != WUPSCONFIG_API_RESULT_SUCCESS) {
        DEBUG_FUNCTION_LINE_ERR("WUPSConfigAPI_Init failed: %s", WUPSConfigAPI_GetStatusStr(status));
    }
}

} // namespace StreamConfig
