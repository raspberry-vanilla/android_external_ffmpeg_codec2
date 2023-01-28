#ifndef FFMPEG_HWACCEL_H
#define FFMPEG_HWACCEL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "libavcodec/avcodec.h"

extern int  ffmpeg_hwaccel_init(AVCodecContext *avctx);
extern void ffmpeg_hwaccel_deinit(AVCodecContext *avctx);
extern int  ffmpeg_hwaccel_get_frame(AVCodecContext *avctx, AVFrame *frame);

#ifdef __cplusplus
};
#endif

#endif
