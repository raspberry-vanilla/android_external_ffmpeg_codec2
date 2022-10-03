#define DEBUG_HWACCEL 0
#define LOG_TAG "HWACCEL"
#include <cutils/log.h>
#include <cutils/properties.h>

#include "config.h"
#include "ffmpeg_hwaccel.h"
#include "libavutil/opt.h"

int ffmpeg_hwaccel_init(AVCodecContext *avctx) {
    if (!property_get_bool("media.sf.hwaccel", 0))
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
        if (av_hwdevice_ctx_create(&avctx->hw_device_ctx, config->device_type, "android", NULL, 0) < 0) {
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

int ffmpeg_hwaccel_get_frame(AVCodecContext *avctx __unused, AVFrame *frame) {
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

    err = av_hwframe_transfer_data(output, frame, 0);
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
