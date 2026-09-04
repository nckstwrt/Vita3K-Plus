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

#include <codec/state.h>

#include <util/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cassert>

void copy_yuv_data_from_frame(AVFrame *frame, uint8_t *dest, const uint32_t width, const uint32_t height, bool is_p3) {
    for (size_t i = 0; i < height; i++) {
        memcpy(dest, &frame->data[0][frame->linesize[0] * i], width);
        dest += width;
    }

    if (is_p3) {
        for (size_t i = 0; i < height / 2; i++) {
            memcpy(dest, &frame->data[1][frame->linesize[1] * i], width / 2);
            dest += width / 2;
        }
        for (size_t i = 0; i < height / 2; i++) {
            memcpy(dest, &frame->data[2][frame->linesize[2] * i], width / 2);
            dest += width / 2;
        }
    } else {
        // p2 format, U and V are interleaved
        for (size_t i = 0; i < height / 2; i++) {
            const uint8_t *src_u = &frame->data[1][frame->linesize[1] * i];
            const uint8_t *src_v = &frame->data[2][frame->linesize[2] * i];
            for (size_t j = 0; j < width / 2; j++) {
                dest[0] = src_u[j];
                dest[1] = src_v[j];
                dest += 2;
            }
        }
    }
}

static void reset_h264_parser(AVCodecParserContext *&parser) {
    if (parser)
        av_parser_close(parser);
    parser = av_parser_init(AV_CODEC_ID_H264);
    if (parser)
        parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
}

static bool receive_h264_frame(H264DecoderState &decoder, uint8_t *data, DecoderSize *size, bool warn_if_unavailable) {
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR("Error allocating H264 frame.");
        return false;
    }

    const int error = avcodec_receive_frame(decoder.context, frame);
    if (error < 0) {
        if (warn_if_unavailable || (error != AVERROR(EAGAIN) && error != AVERROR_EOF))
            LOG_WARN("Error receiving H264 frame: {}.", codec_error_name(error));
        av_frame_free(&frame);
        return false;
    }

    if (data)
        copy_yuv_data_from_frame(frame, data, decoder.width_in, decoder.height_in, decoder.output_yuvp3);

    if (size)
        *size = { { static_cast<uint32_t>(decoder.context->width), static_cast<uint32_t>(decoder.context->height) } };

    decoder.width_out = frame->width;
    decoder.height_out = frame->height;
    decoder.pts_out = frame->pts;

    av_frame_free(&frame);
    return true;
}

uint32_t H264DecoderState::buffer_size(DecoderSize size) {
    return size.width * size.height * 3 / 2;
}

uint32_t H264DecoderState::get(DecoderQuery query) {
    switch (query) {
    case DecoderQuery::WIDTH: return context->width;
    case DecoderQuery::HEIGHT: return context->height;
    default: return 0;
    }
}

uint64_t H264DecoderState::au_hash(const uint8_t *p, uint32_t n) {
    // FNV-1a: an access unit is at most a few tens of KB and this runs once per decode call
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h ^ n;
}

bool H264DecoderState::send(const uint8_t *data, uint32_t size) {
    std::lock_guard<std::mutex> lock(codec_mutex);
    last_au_hash = au_hash(data, size);
    last_send_was_refeed = false;
    for (const SentAu &s : recent_sent)
        if (s.hash == last_au_hash) {
            last_send_was_refeed = true;
            break;
        }
    recent_sent.push_back({ pts, last_au_hash });
    if (recent_sent.size() > 16)
        recent_sent.erase(recent_sent.begin());

    // A decoder that received an end-of-stream packet rejects new input until it is reset. Keep
    // the Vita decoder reusable if a title starts a new sequence after DecodeStop without an
    // explicit DecodeFlush.
    if (is_draining) {
        avcodec_flush_buffers(context);
        reset_h264_parser(parser);
        is_draining = false;
    }

    int error = 0;

    std::vector<uint8_t> au_frame(size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(au_frame.data(), data, size);

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        LOG_ERROR("Error allocating H264 packet.");
        return false;
    }
    error = av_parser_parse2(
        parser, // AVCodecParserContext *s,
        context, // AVCodecContext *avctx,
        &packet->data, // uint8_t **poutbuf,
        &packet->size, // int *poutbuf_size,
        au_frame.data(), // const uint8_t *buf,
        size, // int buf_size,
        pts == ~0ull ? AV_NOPTS_VALUE : pts, // int64_t pts,
        dts == ~0ull ? AV_NOPTS_VALUE : dts, // int64_t dts,
        0 // int64_t pos
    );
    if (error < 0) {
        LOG_WARN("Error parsing H264 packet: {}.", codec_error_name(error));
        av_packet_free(&packet);
        return false;
    }

    packet->pts = parser->pts;
    packet->dts = parser->dts;

    error = avcodec_send_packet(context, packet);
    av_packet_free(&packet);
    if (error < 0) {
        LOG_WARN("Error sending H264 packet: {}.", codec_error_name(error));
        return false;
    }

    return true;
}

bool H264DecoderState::receive(uint8_t *data, DecoderSize *size) {
    std::lock_guard<std::mutex> lock(codec_mutex);
    return receive_h264_frame(*this, data, size, true);
}

bool H264DecoderState::drain(uint8_t *data, DecoderSize *size) {
    std::lock_guard<std::mutex> lock(codec_mutex);

    if (!context)
        return false;

    if (!is_draining) {
        const int error = avcodec_send_packet(context, nullptr);
        if (error == 0 || error == AVERROR_EOF) {
            is_draining = true;
        } else if (error != AVERROR(EAGAIN)) {
            LOG_WARN("Error draining H264 decoder: {}.", codec_error_name(error));
            return false;
        }
    }

    return receive_h264_frame(*this, data, size, false);
}

void H264DecoderState::configure(void *options) {
    auto *opt = static_cast<H264DecoderOptions *>(options);

    pts = static_cast<uint64_t>(opt->pts_upper) << 32u | static_cast<uint64_t>(opt->pts_lower);
    dts = static_cast<uint64_t>(opt->dts_upper) << 32u | static_cast<uint64_t>(opt->dts_lower);
}

void H264DecoderState::set_res(const uint32_t width, const uint32_t height) {
    width_in = width;
    height_in = height;
}

void H264DecoderState::get_res(uint32_t &width, uint32_t &height) {
    width = width_out;
    height = height_out;
}

void H264DecoderState::get_pts(uint32_t &upper, uint32_t &lower) {
    upper = pts_out >> 32u;
    lower = pts_out & 0xFFFFFFFF;
}

void H264DecoderState::set_output_format(bool is_yuv_p3) {
    this->output_yuvp3 = is_yuv_p3;
}

H264DecoderState::H264DecoderState(uint32_t width, uint32_t height) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    assert(codec);

    parser = av_parser_init(codec->id);
    assert(parser);
    parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

    context = avcodec_alloc_context3(codec);
    assert(context);
    context->width = width;
    context->height = height;

    int result = avcodec_open2(context, codec, nullptr);
    assert(result == 0);
}

H264DecoderState::~H264DecoderState() {
    av_parser_close(parser);
}

static void hold_insert(std::vector<H264DecoderState::HeldPicture> &held, H264DecoderState::HeldPicture &&pic);

void H264DecoderState::flush() {
    std::lock_guard<std::mutex> lock(codec_mutex);
    // Before discarding, drain whatever FFmpeg is still holding for reordering and keep the last picture
    if (context && !is_draining && width_in && height_in) {
        const int error = avcodec_send_packet(context, nullptr);
        if (error == 0 || error == AVERROR_EOF || error == AVERROR(EAGAIN)) {
            // Drain everything
            std::vector<HeldPicture> drained;
            const size_t bytes = buffer_size({ { width_in, height_in } });
            for (;;) {
                HeldPicture pic;
                pic.data.resize(bytes);
                if (!receive_h264_frame(*this, pic.data.data(), nullptr, false))
                    break;
                pic.pts = pts_out;
                pic.width = width_out;
                pic.height = height_out;
                pic.yuvp3 = output_yuvp3;
                drained.push_back(std::move(pic));
                if (drained.size() > 8)
                    break;
            }
            for (HeldPicture &pic : drained) {
                pic.au_hash = hash_for_pts(pic.pts);
                if (!pic.au_hash && drained.size() == 1)
                    pic.au_hash = last_au_hash;
                if (pic.au_hash)
                    hold_insert(held, std::move(pic));
            }
        }
    }
    if (context)
        avcodec_flush_buffers(context);
    is_draining = false;
    reset_h264_parser(parser);
}

uint64_t H264DecoderState::hash_for_pts(uint64_t pic_pts) const {
    if (pic_pts == ~0ull)
        return 0;
    for (auto it = recent_sent.rbegin(); it != recent_sent.rend(); ++it)
        if (it->pts == pic_pts)
            return it->hash;
    return 0;
}

static void hold_insert(std::vector<H264DecoderState::HeldPicture> &held, H264DecoderState::HeldPicture &&pic) {
    for (auto &h : held)
        if (h.au_hash == pic.au_hash) {
            h = std::move(pic);
            return;
        }
    if (held.size() >= 4)
        held.erase(held.begin());
    held.push_back(std::move(pic));
}

void H264DecoderState::stash_picture(const uint8_t *data, uint64_t pic_pts, uint32_t width, uint32_t height, bool yuvp3) {
    std::lock_guard<std::mutex> lock(codec_mutex);
    const uint64_t key = hash_for_pts(pic_pts);
    if (!key || !data)
        return;
    HeldPicture pic;
    pic.au_hash = key;
    pic.pts = pic_pts;
    pic.width = width;
    pic.height = height;
    pic.yuvp3 = yuvp3;
    pic.data.assign(data, data + buffer_size({ { width_in, height_in } }));
    hold_insert(held, std::move(pic));
}

bool H264DecoderState::take_held_picture(uint8_t *out, uint64_t hash, uint32_t width, uint32_t height, bool yuvp3) {
    std::lock_guard<std::mutex> lock(codec_mutex);
    for (auto it = held.begin(); it != held.end(); ++it) {
        if (it->au_hash != hash)
            continue;
        if (it->yuvp3 != yuvp3 || width_in != width || height_in != height || it->data.size() != buffer_size({ { width, height } }))
            return false;
        if (out)
            memcpy(out, it->data.data(), it->data.size());
        width_out = it->width;
        height_out = it->height;
        pts_out = it->pts;
        held.erase(it);
        return true;
    }
    return false;
}

bool H264DecoderState::poll(uint8_t *data) {
    std::lock_guard<std::mutex> lock(codec_mutex);
    if (!context)
        return false;
    return receive_h264_frame(*this, data, nullptr, false);
}
