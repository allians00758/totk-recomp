// hwopus: the hardware Opus decoder (nn::codec::HardwareOpusDecoder). TOTK decodes its voices and some of its sounds
// with it; BotW didn't use it (Savage Wind only had a logging stub, whose missing decoder object the game then called:
// DecodeOpusInterleavedWithHardware, "accessing 0"). Decoded here with FFmpeg's Opus decoder (the one the movies'
// library already has), to 16-bit interleaved samples. A packet it can't decode gives as many samples of silence as
// the packet holds, so the game's audio keeps its timing.
//
// IHardwareOpusDecoderManager: 0 Open(params), 1 GetWorkBufferSize(params), 4 OpenEx(params ex), 5 GetWorkBufferSizeEx,
// 8 GetWorkBufferSizeExEx (HOS 17+); 2/3/6/7/9 the multistream ones (not supported yet: logged, an error).
// params: s32 sample rate, s32 channels; params ex: + u8 use_large_frame_size, padding (0x10 bytes).
// IHardwareOpusDecoder: 0/4/6/8 DecodeInterleaved (old, with perf, with perf and reset, current: reset flag in),
// in buffer A: packets of {u32 size BE, u32 final range BE, Opus data}; out buffer B: s16 samples;
// out: u32 bytes consumed, u32 samples per channel, u64 time taken (perf) - 1 SetContext.
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../ipc.h"
#include "../hle.h"
#include "../rt.h"

#define R_OPUS_INVALID MAKE_RESULT(111, 1000)  // (codec module: any non-zero result fails the call)

typedef struct {
    int rate, channels;
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    uint64_t packets, failures;
} Opus;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Samples per channel at 48 kHz an Opus packet holds (RFC 6716 3.1: the TOC byte's configuration and frame count)
static int packet_samples(const uint8_t *d, uint32_t n) {
    if (!n) return 0;
    int cfg = d[0] >> 3, per;
    if (cfg < 12) per = (int[]){480, 960, 1920, 2880}[cfg & 3];  // SILK: 10, 20, 40, 60 ms
    else if (cfg < 16) per = (cfg & 1) ? 960 : 480;               // hybrid: 10, 20 ms
    else per = (int[]){120, 240, 480, 960}[cfg & 3];              // CELT: 2.5, 5, 10, 20 ms
    int frames = (d[0] & 3) == 0 ? 1 : (d[0] & 3) < 3 ? 2 : n > 1 ? (d[1] & 0x3f) : 0;
    return per * frames;
}

static void opus_free(Service *s) {
    Opus *o = s->state;
    if (!o) return;
    LOG("[hwopus] decoder closed: %llu packets, %llu not decoded\n", (unsigned long long)o->packets,
        (unsigned long long)o->failures);
    avcodec_free_context(&o->ctx);
    av_packet_free(&o->pkt);
    av_frame_free(&o->frame);
    free(o);
    s->state = NULL;
}

// One packet: its samples at the decoder's rate, interleaved 16-bit, into out (room for max samples per channel).
// Returns samples per channel (silence for a packet FFmpeg doesn't decode).
static int decode_packet(Opus *o, const uint8_t *data, uint32_t size, int16_t *out, int max) {
    int step = o->rate > 0 && o->rate < 48000 ? 48000 / o->rate : 1;  // (FFmpeg decodes at 48 kHz: every step-th one)
    int want = packet_samples(data, size) / step;
    if (want > max) want = max;
    o->packets++;
    int got = 0;
    if (o->ctx) {
        o->pkt->data = (uint8_t *)data;
        o->pkt->size = (int)size;
        if (avcodec_send_packet(o->ctx, o->pkt) == 0) {
            while (avcodec_receive_frame(o->ctx, o->frame) == 0) {
                AVFrame *f = o->frame;
                int ch = f->ch_layout.nb_channels < o->channels ? f->ch_layout.nb_channels : o->channels;
                for (int i = 0; i < f->nb_samples && got < max; i += step, got++)
                    for (int c = 0; c < o->channels; c++) {
                        float v = 0;
                        int cc = c < ch ? c : ch - 1;
                        if (cc >= 0) {
                            if (f->format == AV_SAMPLE_FMT_FLTP) v = ((const float *)f->extended_data[cc])[i];
                            else if (f->format == AV_SAMPLE_FMT_FLT) v = ((const float *)f->data[0])[i * ch + cc];
                            else if (f->format == AV_SAMPLE_FMT_S16P) v = ((const int16_t *)f->extended_data[cc])[i] / 32768.0f;
                            else if (f->format == AV_SAMPLE_FMT_S16) v = ((const int16_t *)f->data[0])[i * ch + cc] / 32768.0f;
                        }
                        int s = (int)(v * 32767.0f);
                        out[got * o->channels + c] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
                    }
                av_frame_unref(f);
            }
        }
    }
    if (got < want) {  // (not decoded, or less than the packet holds: the rest is silence)
        if (!got) o->failures++;
        memset(out + got * o->channels, 0, sizeof *out * (size_t)(want - got) * (size_t)o->channels);
        got = want;
    }
    return got;
}

static void decoder(Service *s, IpcReq *req, IpcResp *resp) {
    Opus *o = s->state;
    switch (req->cmd) {
    case 0: case 4: case 6: case 8: {  // DecodeInterleaved (old / with perf / with perf and reset / current)
        if ((req->cmd == 6 || req->cmd == 8) && req->data_size >= 1 && IN(uint8_t, 0) && o->ctx) avcodec_flush_buffers(o->ctx);
        IpcBuf *in = in_buf(req, 0), *ob = out_buf(req, 0);
        uint32_t consumed = 0, samples = 0;
        if (in && ob && in->size >= 8) {
            const uint8_t *d = (const uint8_t *)(uintptr_t)in->addr;
            uint32_t n = be32(d);
            if (8 + (uint64_t)n > in->size) { resp->result = R_OPUS_INVALID; break; }
            int max = (int)(ob->size / (2u * (uint32_t)o->channels));
            samples = (uint32_t)decode_packet(o, d + 8, n, (int16_t *)(uintptr_t)ob->addr, max);
            consumed = 8 + n;
        }
        OUT(uint32_t, 0) = consumed;
        OUT(uint32_t, 4) = samples;
        if (req->cmd == 0) { resp_size(resp, 8); break; }
        OUT(uint64_t, 8) = 0;  // (time taken, microseconds)
        resp_size(resp, 16);
        break;
    }
    case 1: break;  // SetContext
    default: LOG("[hwopus] IHardwareOpusDecoder cmd %u (ok)\n", req->cmd);
    }
}

static Service *open_decoder(int rate, int channels) {
    Opus *o = calloc(1, sizeof *o);
    o->rate = rate;
    o->channels = channels < 1 ? 1 : channels > 2 ? 2 : channels;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (codec && (o->ctx = avcodec_alloc_context3(codec))) {
        o->ctx->sample_rate = 48000;
        av_channel_layout_default(&o->ctx->ch_layout, o->channels);
        if (avcodec_open2(o->ctx, codec, NULL) < 0) avcodec_free_context(&o->ctx);
    }
    o->pkt = av_packet_alloc();
    o->frame = av_frame_alloc();
    LOG("[hwopus] decoder %d Hz, %d channel(s)%s\n", rate, o->channels, o->ctx ? "" : ": no Opus decoder in FFmpeg, silence");
    Service *s = service_new("IHardwareOpusDecoder", decoder, o);
    s->on_close = opus_free;
    return s;
}

SERVICE("hwopus", hwopus) {
    (void)s;
    switch (req->cmd) {
    case 0: case 4:  // OpenHardwareOpusDecoder(Ex)(params, work buffer size, transfer memory)
        ipc_return_object(resp, open_decoder(IN(int32_t, 0), IN(int32_t, 4)));
        break;
    case 1: case 5: case 8: {  // GetWorkBufferSize(Ex)(ExEx)(params): any size works here (the memory isn't used)
        int ch = IN(int32_t, 4);
        OUT(uint32_t, 0) = 0x4000u + 0x8000u * (uint32_t)(ch < 1 ? 1 : ch);
        resp_size(resp, 4);
        break;
    }
    case 3: case 7: case 9:  // GetWorkBufferSizeForMultiStream(Ex)(ExEx)
        OUT(uint32_t, 0) = 0x40000u;
        resp_size(resp, 4);
        break;
    default:  // (the multistream decoders: not yet)
        LOG("[hwopus] cmd %u not supported\n", req->cmd);
        resp->result = R_OPUS_INVALID;
    }
}
