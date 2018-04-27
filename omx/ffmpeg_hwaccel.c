#define DEBUG_HWACCEL 0
#define LOG_TAG "HWACCEL"
#include <cutils/log.h>
#include <cutils/properties.h>

#include "config.h"
#include "ffmpeg_hwaccel.h"
#include "libavutil/hwcontext.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#ifdef LIBAV_CONFIG_H
#include "avtools/avconv.h"
#else
#include "ffmpeg.h"
#endif

/* BEGIN: Extracted from ffmpeg_opt.c */

const HWAccel hwaccels[] = {
#if HAVE_VDPAU_X11
    { "vdpau", vdpau_init, HWACCEL_VDPAU, AV_PIX_FMT_VDPAU },
#endif
#if CONFIG_VDA
    { "vda", videotoolbox_init, HWACCEL_VDA, AV_PIX_FMT_VDA },
#endif
#if CONFIG_LIBMFX
    { "qsv", qsv_init, HWACCEL_QSV, AV_PIX_FMT_QSV },
#endif
#if CONFIG_VAAPI
#ifdef LIBAV_CONFIG_H
    { "vaapi", hwaccel_decode_init, HWACCEL_VAAPI, AV_PIX_FMT_VAAPI, AV_HWDEVICE_TYPE_VAAPI },
#else
    { "vaapi", vaapi_decode_init, HWACCEL_VAAPI, AV_PIX_FMT_VAAPI },
#endif
#endif
#if CONFIG_CUVID
    { "cuvid", cuvid_init, HWACCEL_CUVID, AV_PIX_FMT_CUDA },
#endif
    { 0, 0, HWACCEL_NONE, AV_PIX_FMT_NONE },
};

/* END: Extracted from ffmpeg_opt.c */

int ffmpeg_hwaccel_get_frame(AVCodecContext *avctx, AVFrame *frame)
{
    int err;
    InputStream *ist = avctx->opaque;

    if (ist && ist->hwaccel_retrieve_data && frame->format == ist->hwaccel_pix_fmt) {
        ALOGV_IF(DEBUG_HWACCEL, "ffmpeg_hwaccel_get_frame ctx=%p ist=%p format=%d hwaccel_pix_fmt=%d hwaccel_id=%d",
                avctx, ist, frame->format, ist->hwaccel_pix_fmt, ist->hwaccel_id);
        err = ist->hwaccel_retrieve_data(avctx, frame);
        if (err == 0)
            ist->hwaccel_retrieved_pix_fmt = frame->format;
    } else {
        err = 0;
    }

    return err;
}

/* BEGIN: Extracted from ffmpeg.c */

static const HWAccel *get_hwaccel(enum AVPixelFormat pix_fmt)
{
    int i;
    for (i = 0; hwaccels[i].name; i++)
        if (hwaccels[i].pix_fmt == pix_fmt)
            return &hwaccels[i];
    return NULL;
}

static enum AVPixelFormat get_format(AVCodecContext *s, const enum AVPixelFormat *pix_fmts)
{
    InputStream *ist = s->opaque;
    const enum AVPixelFormat *p;
    int ret = -1;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        ALOGD_IF(DEBUG_HWACCEL, "check %td pix_fmts=%d", p - pix_fmts, *p);
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        const HWAccel *hwaccel;

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            break;

        hwaccel = get_hwaccel(*p);
        if (!hwaccel ||
                (ist->active_hwaccel_id && ist->active_hwaccel_id != hwaccel->id) ||
                (ist->hwaccel_id != HWACCEL_AUTO && ist->hwaccel_id != hwaccel->id))
            continue;

        ret = hwaccel->init(s);
        if (ret < 0) {
            if (ist->hwaccel_id == hwaccel->id) {
                av_log(NULL, AV_LOG_FATAL,
                       "%s hwaccel requested for input stream #%d, "
                       "but cannot be initialized.\n", hwaccel->name, ist->file_index);
            }
            continue;
        }
        if (ist->hw_frames_ctx) {
            s->hw_frames_ctx = av_buffer_ref(ist->hw_frames_ctx);
            if (!s->hw_frames_ctx)
                return AV_PIX_FMT_NONE;
        }
        ist->active_hwaccel_id = hwaccel->id;
        ist->hwaccel_pix_fmt = *p;
        s->thread_count = 1;
#ifdef LIBAV_CONFIG_H
        ist->dec = s->codec;
        ist->dec_ctx = avcodec_alloc_context3(ist->dec);
        avcodec_copy_context(ist->dec_ctx, s);
        ret = hw_device_setup_for_decode(ist);
        s->hw_device_ctx = ist->dec_ctx->hw_device_ctx;
        ALOGD_IF(DEBUG_HWACCEL, "hw_device_setup_for_decode: %d ctx=%p hw_device_ctx=%p pix_fmts=%d", ret, s, s->hw_device_ctx, *p);
#endif
        break;
    }

    ALOGI("hw codec %s %sabled: s=%p ist=%p pix_fmts=%d", avcodec_get_name(s->codec_id), ret ? "dis" : "en", s, ist, *p);
    return *p;
}

static int get_buffer(AVCodecContext *s, AVFrame *frame, int flags)
{
    InputStream *ist = s->opaque;
    ALOGV_IF(DEBUG_HWACCEL, "get_buffer: s=%p ist=%p format=%d hwaccel_pix_fmt=%d",
            s, ist, frame->format, ist->hwaccel_pix_fmt);

    if (ist->hwaccel_get_buffer && frame->format == ist->hwaccel_pix_fmt)
        return ist->hwaccel_get_buffer(s, frame, flags);

    return avcodec_default_get_buffer2(s, frame, flags);
}

/* END: Extracted from ffmpeg.c */

int ffmpeg_hwaccel_init(AVCodecContext *avctx)
{
    if (!property_get_bool("media.sf.hwaccel", 0))
        return 0;

    InputStream *ist = av_mallocz(sizeof(*ist));
    if (!ist)
        return AVERROR(ENOMEM);

    ist->hwaccel_id = HWACCEL_AUTO;
    ist->hwaccel_device = "android";
    ist->hwaccel_output_format = AV_PIX_FMT_YUV420P;

    avctx->opaque = ist;
    avctx->get_format = get_format;
    avctx->get_buffer2 = get_buffer;
    avctx->thread_safe_callbacks = 1;
    av_opt_set_int(avctx, "refcounted_frames", 1, 0);

    ALOGD_IF(DEBUG_HWACCEL, "ffmpeg_hwaccel_init ctx=%p ist=%p", avctx, ist);
    return 0;
}

void ffmpeg_hwaccel_deinit(AVCodecContext *avctx)
{
    if (avctx->opaque) {
        InputStream *ist = avctx->opaque;
        if (ist->hwaccel_uninit)
            ist->hwaccel_uninit(avctx);
#ifdef LIBAV_CONFIG_H
        avcodec_free_context(&ist->dec_ctx);
#endif
        av_freep(&avctx->opaque);
    }
}
