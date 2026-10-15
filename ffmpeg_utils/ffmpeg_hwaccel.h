#ifndef FFMPEG_HWACCEL_H
#define FFMPEG_HWACCEL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "libavcodec/avcodec.h"

extern int  ffmpeg_hwaccel_init(AVCodecContext *avctx);
extern void ffmpeg_hwaccel_deinit(AVCodecContext *avctx);
/* allow_10bit: keep 10-bit HEVC (Main 10) output as P010LE (if libavutil
 * supports it) or YUV420P10LE instead of truncating it to 8-bit NV12. */
extern int  ffmpeg_hwaccel_get_frame(AVCodecContext *avctx, AVFrame *frame, int allow_10bit);

/* NV12 -> planar 4:2:0 (YV12 / I420 depending on the dst_u / dst_v pointers),
 * multithreaded. Returns 0 on success or -1 if the size is not supported
 * (odd width or height), in which case nothing was written. */
extern int  ffmpeg_nv12_to_yuv420p(const uint8_t *src_y, int src_y_stride,
                                   const uint8_t *src_uv, int src_uv_stride,
                                   uint8_t *dst_y, int dst_y_stride,
                                   uint8_t *dst_u, int dst_u_stride,
                                   uint8_t *dst_v, int dst_v_stride,
                                   int w, int h, int nthreads);

/* Converts a hardware (DRM PRIME, SAND) frame straight into NV12 planes owned by
 * the caller (luma plane + interleaved CbCr plane), w x h from the top left.
 * Returns 0 on success, negative on error (nothing usable written). */
extern int  ffmpeg_hwaccel_nv12_into(AVFrame *hwframe,
                                     uint8_t *dst_y, int dst_stride_y,
                                     uint8_t *dst_c, int dst_stride_c,
                                     int w, int h);

/* Maximum number of threads used for the SAND conversions (1..4). */
extern void ffmpeg_hwaccel_set_threads(int n);

#ifdef __cplusplus
};
#endif

#endif
