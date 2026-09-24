// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <module/module.h>

#include <codec/state.h>
#include <codec/types.h>
#include <kernel/thread/thread_state.h>
#include <renderer/functions.h>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceJpegEncUser);

struct SceJpegEncoderContext {
    int32_t inWidth;
    int32_t inHeight;
    int32_t pixelFormat;
    Ptr<uint8_t> outBuffer;
    uint32_t outSize;
    SceJpegEncoderInitParamOption option;

    int32_t compressRatio;
    int32_t headerMode;

    int32_t validWidth;
    int32_t validHeight;
};

static int sceJpegEncoderInitImpl(SceJpegEncoderContext *context, int32_t inWidth, int32_t inHeight, int32_t pixelFormat, Ptr<uint8_t> outBuffer, uint32_t outSize, SceJpegEncoderInitParamOption option = SCE_JPEGENC_INIT_PARAM_OPTION_NONE) {
    context->inWidth = inWidth;
    context->inHeight = inHeight;
    context->pixelFormat = pixelFormat;
    context->outBuffer = outBuffer;
    context->outSize = outSize;
    context->option = option;

    context->compressRatio = 64;
    context->headerMode = SCE_JPEGENC_HEADER_MODE_JPEG;

    context->validWidth = inWidth;
    context->validHeight = inHeight;

    return 0;
}

EXPORT(int, sceJpegEncoderCsc, SceJpegEncoderContext *context, Ptr<uint8_t> outBuffer, Ptr<uint8_t> inBuffer, int32_t inPitch, int32_t inPixelFormat) {
    TRACY_FUNC(sceJpegEncoderCsc, context, outBuffer, inBuffer, inPitch, inPixelFormat);
    if (!context || !outBuffer.valid(emuenv.mem) || !inBuffer.valid(emuenv.mem))
        return RET_ERROR(SCE_JPEGENC_ERROR_INVALID_POINTER);

    DecoderColorSpace color_space = COLORSPACE_UNKNOWN;

    switch (context->pixelFormat & 0xF) {
    case SCE_JPEGENC_PIXEL_YCBCR422:
        color_space = COLORSPACE_YUV422P;
        break;
    case SCE_JPEGENC_PIXEL_YCBCR420:
        color_space = COLORSPACE_YUV420P;
        break;
    default:
        return SCE_JPEGENC_ERROR_INVALID_PIXELFORMAT;
    }

    const bool is_bgra = inPixelFormat == SCE_JPEGENC_PIXEL_BGRA8888;
    if (!is_bgra && inPixelFormat != SCE_JPEGENC_PIXEL_RGBA8888)
        return RET_ERROR(SCE_JPEGENC_ERROR_INVALID_PIXELFORMAT);

    guest_sched_release_for_block();
    renderer::sync_guest_range(*emuenv.renderer, inBuffer.address(), static_cast<uint32_t>(inPitch) * 4 * context->inHeight);

    const bool hw_csc_pitch = context->pixelFormat & SCE_JPEGENC_PITCH_HW_CSC;
    convert_rgb_to_yuv(inBuffer.get(emuenv.mem), outBuffer.get(emuenv.mem), context->inWidth, context->inHeight, color_space, inPitch, is_bgra, hw_csc_pitch);

    return 0;
}

EXPORT(int, sceJpegEncoderEncode, SceJpegEncoderContext *context, Ptr<uint8_t> inBuffer) {
    TRACY_FUNC(sceJpegEncoderEncode, context, inBuffer);
    if (!context || !inBuffer.valid(emuenv.mem) || !context->outBuffer.valid(emuenv.mem))
        return RET_ERROR(SCE_JPEGENC_ERROR_INVALID_POINTER);

    DecoderColorSpace color_space = COLORSPACE_UNKNOWN;

    switch (context->pixelFormat & 0xF) {
    case SCE_JPEGENC_PIXEL_YCBCR422:
        color_space = COLORSPACE_YUV422P;
        break;
    case SCE_JPEGENC_PIXEL_YCBCR420:
        color_space = COLORSPACE_YUV420P;
        break;
    default:
        return SCE_JPEGENC_ERROR_INVALID_PIXELFORMAT;
    }

    const bool hw_csc_pitch = context->pixelFormat & SCE_JPEGENC_PITCH_HW_CSC;
    uint32_t size = convert_yuv_to_jpeg(inBuffer.get(emuenv.mem), context->outBuffer.get(emuenv.mem), context->inWidth, context->inHeight,
        context->validWidth, context->validHeight, hw_csc_pitch, context->outSize, color_space, context->compressRatio);

    if (size == -1) {
        return SCE_JPEGENC_ERROR_INSUFFICIENT_BUFFER;
    }

    return size;
}

EXPORT(int, sceJpegEncoderEnd, SceJpegEncoderContext *context) {
    TRACY_FUNC(sceJpegEncoderEnd, context);
    return 0;
}

EXPORT(int, sceJpegEncoderGetContextSize) {
    TRACY_FUNC(sceJpegEncoderGetContextSize);
    return sizeof(SceJpegEncoderContext);
}

EXPORT(int, sceJpegEncoderInit, SceJpegEncoderContext *context, int32_t inWidth, int32_t inHeight, int32_t pixelFormat, Ptr<uint8_t> outBuffer, uint32_t outSize) {
    TRACY_FUNC(sceJpegEncoderInit, context, inWidth, inHeight, pixelFormat, outBuffer, outSize);
    return sceJpegEncoderInitImpl(context, inWidth, inHeight, pixelFormat, outBuffer, outSize, SCE_JPEGENC_INIT_PARAM_OPTION_NONE);
}

EXPORT(int, sceJpegEncoderInitWithParam, SceJpegEncoderContext *context, SceJpegEncoderInitParam *initParam) {
    TRACY_FUNC(sceJpegEncoderInitWithParam, context, initParam);
    return sceJpegEncoderInitImpl(context, initParam->inWidth, initParam->inHeight, initParam->pixelFormat, initParam->outBuffer, initParam->outSize, initParam->option);
}

EXPORT(int, sceJpegEncoderSetCompressionRatio, SceJpegEncoderContext *context, int32_t ratio) {
    TRACY_FUNC(sceJpegEncoderSetCompressionRatio, context, ratio);
    if (ratio < 0 || ratio > 255) {
        return SCE_JPEGENC_ERROR_INVALID_COMPRATIO;
    }
    context->compressRatio = ratio;
    return 0;
}

EXPORT(int, sceJpegEncoderSetHeaderMode, SceJpegEncoderContext *context, int32_t mode) {
    TRACY_FUNC(sceJpegEncoderSetHeaderMode, context, mode);
    if (mode != SCE_JPEGENC_HEADER_MODE_JPEG && mode != SCE_JPEGENC_HEADER_MODE_MJPEG) {
        return SCE_JPEGENC_ERROR_INVALID_HEADER_MODE;
    } else if (mode == SCE_JPEGENC_HEADER_MODE_MJPEG) {
        return STUBBED("SCE_JPEGENC_HEADER_MODE_MJPEG is not supported");
    }
    context->headerMode = mode;
    return 0;
}

EXPORT(int, sceJpegEncoderSetOutputAddr, SceJpegEncoderContext *context, Ptr<uint8_t> outBuffer, uint32_t outSize) {
    TRACY_FUNC(sceJpegEncoderSetOutputAddr, context, outBuffer, outSize);
    context->outBuffer = outBuffer;
    context->outSize = outSize;
    return 0;
}

EXPORT(int, sceJpegEncoderSetValidRegion, SceJpegEncoderContext *context, int32_t inWidth, int32_t inHeight) {
    TRACY_FUNC(sceJpegEncoderSetValidRegion, context, inWidth, inHeight);
    if (!context)
        return RET_ERROR(SCE_JPEGENC_ERROR_INVALID_POINTER);

    if (inWidth <= 0 || inHeight <= 0 || inWidth > context->inWidth || inHeight > context->inHeight)
        return RET_ERROR(SCE_JPEGENC_ERROR_IMAGE_SIZE);

    context->validWidth = inWidth;
    context->validHeight = inHeight;
    return 0;
}
