#define DEBUG_HWACCEL 0
#define LOG_TAG "HWACCEL"
#include <cutils/log.h>
#include <cutils/properties.h>

#include "ffmpeg_hwaccel.h"
#include "libavutil/hwcontext.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/rpi_sand_fns.h"

#include <pthread.h>
#include <string.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

// True if the HW frames hold more than 8 bits per sample (RPI4_10 / SAND30 for
// rpi-hevc-dec). More robust than avctx->profile: Main 10 streams may signal
// another general_profile_idc (e.g. RExt or compatibility flags only).
static int hw_frame_is_high_bit_depth(const AVCodecContext *avctx, const AVFrame *frame) {
    const AVHWFramesContext *hwfc = (const AVHWFramesContext *)frame->hw_frames_ctx->data;
    const AVPixFmtDescriptor *desc;

    // The Raspberry Pi SAND formats have no component descriptors (depth 0)
    // in libavutil/pixdesc.c, so they must be special-cased.
    switch (hwfc->sw_format) {
    case AV_PIX_FMT_RPI4_10:
    case AV_PIX_FMT_SAND64_10:
        return 1;
    case AV_PIX_FMT_RPI4_8:
    case AV_PIX_FMT_SAND128:
        return 0;
    default:
        break;
    }
    desc = av_pix_fmt_desc_get(hwfc->sw_format);
    if (desc && desc->comp[0].depth > 0)
        return desc->comp[0].depth > 8;
    return avctx->profile == AV_PROFILE_HEVC_MAIN_10;
}

int ffmpeg_hwaccel_init(AVCodecContext *avctx) {
    if (avctx->codec_id != AV_CODEC_ID_HEVC || !property_get_bool("persist.vendor.ffmpeg_codec2.v4l2.h265", 0))
        return 0;

    // Find codec information. At this point, AVCodecContext.codec may not be
    // set yet, so retrieve our own version using AVCodecContext.codec_id.
    const AVCodec* codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        ALOGE("ffmpeg_hwaccel_init: codec not found = %d", avctx->codec_id);
        return 0;
    }

    // Find a working HW configuration for this codec.
    for (int i = 0;; i++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            // No more HW configs available.
            break;
        }

        // Try to initialize HW device.
        if (av_hwdevice_ctx_create(&avctx->hw_device_ctx, config->device_type, NULL, NULL, 0) < 0) {
            // Initialization failed, skip this HW config.
            ALOGD_IF(DEBUG_HWACCEL, "ffmpeg_hwaccel_init: failed to initialize HW device %s",
                     av_hwdevice_get_type_name(config->device_type));
            continue;
        }

        // Use refcounted frames.
        av_opt_set_int(avctx, "refcounted_frames", 1, 0);
        // Don't use multithreading.
        avctx->thread_count = 1;

        // HW device created, stop here.
        ALOGD("ffmpeg_hwaccel_init: %p [%s], hw device = %s", avctx, codec->name,
              av_hwdevice_get_type_name(config->device_type));
        break;
    }

    if (!avctx->hw_device_ctx) {
        ALOGD("ffmpeg_hwaccel_init: no HW accel found for codec = %s", codec->name);
    }

    return 0;
}

void ffmpeg_hwaccel_deinit(AVCodecContext *avctx __unused) {
}

// Set once libavutil rejected a SAND30 -> P010 transfer (no such path).
static int direct_p010_unsupported = 0;

int ffmpeg_hwaccel_get_frame(AVCodecContext *avctx, AVFrame *frame, int allow_10bit) {
    if (!frame->hw_frames_ctx) {
        // Frame is not hw-accel
        return 0;
    }

    AVFrame* output;
    int err;

    output = av_frame_alloc();
    if (!output) {
        return AVERROR(ENOMEM);
    }

    output->format = AV_PIX_FMT_YUV420P;
    if (hw_frame_is_high_bit_depth(avctx, frame)) {
        // Default: 8-bit NV12 (fast, but drops 2 bits and cannot carry HDR10).
        // With allow_10bit: P010 straight from SAND30 if libavutil supports it
        // (one pass, and the component then only copies lines), otherwise
        // 16-bit planar YUV420P10LE that swscale converts to P010.
        output->format = !allow_10bit ? AV_PIX_FMT_NV12 :
                         (direct_p010_unsupported ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_P010LE);
    }

    err = av_hwframe_transfer_data(output, frame, 0);
    if (err < 0 && output->format == AV_PIX_FMT_P010LE) {
        // libavutil without the SAND30 -> P010 path: remember and fall back.
        ALOGI("ffmpeg_hwaccel_get_frame: direct P010 transfer unsupported, using YUV420P10LE");
        direct_p010_unsupported = 1;
        av_frame_unref(output);
        output->format = AV_PIX_FMT_YUV420P10LE;
        err = av_hwframe_transfer_data(output, frame, 0);
    }
    if (err < 0) {
        ALOGE("ffmpeg_hwaccel_get_frame failed to transfer data: %s (%08x)",
              av_err2str(err), err);
        goto fail;
    }

    err = av_frame_copy_props(output, frame);
    if (err < 0) {
        ALOGE("ffmpeg_hwaccel_get_frame failed to copy frame properties: %s (%08x)",
              av_err2str(err), err);
        goto fail;
    }

    av_frame_unref(frame);
    av_frame_move_ref(frame, output);
    av_frame_free(&output);

    return 0;

fail:
    av_frame_free(&output);
    return err;
}

// ---------------------------------------------------------------------------
// NV12 -> YV12 / I420 for the 8-bit output path.
//
// swscale does this on one thread and needs ~13 ms for a 4K frame on the
// Raspberry Pi 4, which is a large part of the 4K frame budget. The work is a
// row copy for luma and a de-interleave for chroma, so split the frame into
// horizontal bands and run them in parallel.

#define NV12_MAX_THREADS 4

typedef struct Nv12Job {
    const uint8_t *src_y;  int src_y_stride;
    const uint8_t *src_uv; int src_uv_stride;
    uint8_t *dst_y; int dst_y_stride;
    uint8_t *dst_u; int dst_u_stride;
    uint8_t *dst_v; int dst_v_stride;
    int w;
    int y0, y1;     // luma rows [y0, y1); chroma rows [y0/2, y1/2)
    pthread_t tid;
    int started;
} Nv12Job;

static void *nv12_worker(void *arg) {
    const Nv12Job *j = (const Nv12Job *)arg;
    const int cw = j->w / 2;

    for (int y = j->y0; y < j->y1; y++)
        memcpy(j->dst_y + (size_t)y * j->dst_y_stride,
               j->src_y + (size_t)y * j->src_y_stride, (size_t)j->w);

    for (int y = j->y0 / 2; y < j->y1 / 2; y++) {
        const uint8_t *s = j->src_uv + (size_t)y * j->src_uv_stride;
        uint8_t *du = j->dst_u + (size_t)y * j->dst_u_stride;
        uint8_t *dv = j->dst_v + (size_t)y * j->dst_v_stride;
        int x = 0;
#ifdef __ARM_NEON
        for (; x + 16 <= cw; x += 16) {
            const uint8x16x2_t v = vld2q_u8(s + 2 * x);
            vst1q_u8(du + x, v.val[0]);
            vst1q_u8(dv + x, v.val[1]);
        }
#endif
        for (; x < cw; x++) {
            du[x] = s[2 * x];
            dv[x] = s[2 * x + 1];
        }
    }
    return NULL;
}

int ffmpeg_nv12_to_yuv420p(const uint8_t *src_y, int src_y_stride,
                           const uint8_t *src_uv, int src_uv_stride,
                           uint8_t *dst_y, int dst_y_stride,
                           uint8_t *dst_u, int dst_u_stride,
                           uint8_t *dst_v, int dst_v_stride,
                           int w, int h, int nthreads) {
    Nv12Job job[NV12_MAX_THREADS];
    int n = 0;

    if (w <= 0 || h <= 0 || (w & 1) || (h & 1))
        return -1;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > NV12_MAX_THREADS) nthreads = NV12_MAX_THREADS;

    // Even number of rows per band so that luma and chroma rows line up
    const int band = (((h + nthreads - 1) / nthreads) + 1) & ~1;

    for (int r = 0; r < h && n < NV12_MAX_THREADS; r += band, n++) {
        Nv12Job *j = &job[n];
        j->src_y = src_y;   j->src_y_stride = src_y_stride;
        j->src_uv = src_uv; j->src_uv_stride = src_uv_stride;
        j->dst_y = dst_y;   j->dst_y_stride = dst_y_stride;
        j->dst_u = dst_u;   j->dst_u_stride = dst_u_stride;
        j->dst_v = dst_v;   j->dst_v_stride = dst_v_stride;
        j->w = w;
        j->y0 = r;
        j->y1 = (r + band < h) ? r + band : h;
        j->started = 0;
    }

    // Band 0 runs on the calling thread; if a thread cannot be created its
    // band is converted by the caller.
    for (int i = 1; i < n; i++)
        job[i].started = pthread_create(&job[i].tid, NULL, nv12_worker, &job[i]) == 0;
    nv12_worker(&job[0]);
    for (int i = 1; i < n; i++) {
        if (job[i].started)
            pthread_join(job[i].tid, NULL);
        else
            nv12_worker(&job[i]);
    }
    return 0;
}

int ffmpeg_hwaccel_nv12_into(AVFrame *hwframe,
                             uint8_t *dst_y, int dst_stride_y,
                             uint8_t *dst_c, int dst_stride_c,
                             int w, int h) {
    AVFrame *map = av_frame_alloc();
    int err;

    if (!map)
        return AVERROR(ENOMEM);

    // Map the frame's dmabufs for reading (default format: the SAND layout)
    map->format = AV_PIX_FMT_NONE;
    err = av_hwframe_map(map, hwframe, AV_HWFRAME_MAP_READ);
    if (err < 0) {
        ALOGE("ffmpeg_hwaccel_nv12_into: failed to map frame: %s (%08x)", av_err2str(err), err);
        goto done;
    }

    if (!av_rpi_is_sand_frame(map)) {
        err = AVERROR(EINVAL);
        goto done;
    }

    // Cropping is handled by the w x h region
    map->crop_top = 0;
    map->crop_bottom = 0;
    map->crop_left = 0;
    map->crop_right = 0;

    if (w > map->width)
        w = map->width;
    if (h > map->height)
        h = map->height;

    err = av_rpi_sand_to_nv12_planes(dst_y, dst_stride_y, dst_c, dst_stride_c, map, w, h);
    if (err != 0)
        err = AVERROR(EINVAL);

done:
    av_frame_free(&map);
    return err;
}

void ffmpeg_hwaccel_set_threads(int n) {
    av_rpi_sand_set_max_threads(n > 0 ? (unsigned int)n : 1u);
}
