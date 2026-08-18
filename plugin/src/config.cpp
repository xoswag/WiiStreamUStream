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
constexpr const char *KEY_ENCODER_CORE = "encoderCore";

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
};

ConfigItemMultipleValuesPair sColorValues[] = {
        {WUPS_STREAMING_COLOR_AUTO, "Auto (sRGB correct)"},
        {WUPS_STREAMING_COLOR_RAW, "Raw"},
};

ConfigItemMultipleValuesPair sCoreValues[] = {
        {2, "Core 2"},
        {0, "Core 0"},
        {1, "Core 1"},
};

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

void encoderCoreChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    const int32_t core = (int32_t) newValue;
    if (core == gEncoderCore) {
        return;
    }
    gEncoderCore = core;
    storeU32(KEY_ENCODER_CORE, gEncoderCore);

    // Affinity is fixed when the thread is created, so this is the one setting
    // that needs the encoder rebuilt. Everything else is read per frame.
    if (ImageEncoder::IsRunning()) {
        ImageEncoder::Stop();
        ImageEncoder::Start();
    }
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
                indexOfValue(sCaptureSizeValues, 5, WUPS_STREAMING_SIZE_NATIVE),
                indexOfValue(sCaptureSizeValues, 5, gCaptureSize),
                sCaptureSizeValues, 5, &captureSizeChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
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
                1, gFrameSkip, STREAM_FRAMESKIP_MIN, STREAM_FRAMESKIP_MAX,
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
                rootHandle, KEY_ENCODER_CORE, "Encoder core",
                indexOfValue(sCoreValues, 3, 2),
                indexOfValue(sCoreValues, 3, gEncoderCore),
                sCoreValues, 3, &encoderCoreChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
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
    gCaptureSize = loadU32(KEY_CAPTURE_SIZE, WUPS_STREAMING_SIZE_NATIVE, 0, WUPS_STREAMING_SIZE_240P);
    gColorMode   = loadU32(KEY_COLOR_MODE, WUPS_STREAMING_COLOR_AUTO, 0, 1);
    gQuality     = loadU32(KEY_QUALITY, 55, STREAM_QUALITY_MIN, STREAM_QUALITY_MAX);
    gFrameSkip   = loadU32(KEY_FRAME_SKIP, 1, STREAM_FRAMESKIP_MIN, STREAM_FRAMESKIP_MAX);
    gEncoderCore = loadU32(KEY_ENCODER_CORE, 2, 0, 2);

    WUPSConfigAPIOptionsV1 options = {.name = "Screen Streaming"};
    const WUPSConfigAPIStatus status = WUPSConfigAPI_Init(options, menuOpened, menuClosed);
    if (status != WUPSCONFIG_API_RESULT_SUCCESS) {
        DEBUG_FUNCTION_LINE_ERR("WUPSConfigAPI_Init failed: %s", WUPSConfigAPI_GetStatusStr(status));
    }
}

} // namespace StreamConfig
