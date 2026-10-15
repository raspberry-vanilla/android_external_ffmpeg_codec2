/*
 * Copyright 2022 Michael Goffioul <michael.goffioul@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "C2FFMPEGVideoDecodeComponent"
#include <android-base/properties.h>
#include <log/log.h>
#include <algorithm>

#include <SimpleC2Interface.h>
#include "C2FFMPEGVideoDecodeComponent.h"
#include "ffmpeg_hwaccel.h"
#include "libavutil/mastering_display_metadata.h"
#include <sys/resource.h>
#include <unistd.h>

#define DEBUG_FRAMES 0
#define DEBUG_WORKQUEUE 0
#define DEBUG_EXTRADATA 0

namespace android {

C2FFMPEGVideoDecodeComponent::C2FFMPEGVideoDecodeComponent(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2FFMPEGVideoDecodeInterface>& intf)
    : SimpleC2Component(std::make_shared<SimpleInterface<C2FFMPEGVideoDecodeInterface>>(componentInfo->name, 0, intf)),
      mInfo(componentInfo),
      mIntf(intf),
      mCodecID(componentInfo->codecID),
      mCtx(NULL),
      mImgConvertCtx(NULL),
      mFrame(NULL),
      mPacket(NULL),
      mFFMPEGInitialized(false),
      mCodecAlreadyOpened(false),
      mExtradataReady(false),
      mEOSSignalled(false),
      mHalPixelFormat(HAL_PIXEL_FORMAT_YV12),
      mClientPixelFormat(HAL_PIXEL_FORMAT_YV12),
      mP010Mode(0),
      mP010Supported(false),
      mOutput10Bit(false),
      mFrameIsHw(false),
      mDirectNv12(true),
      mColorAspectsSent(false),
      mHdrSent(false),
      mLastAspects(0u),
      mLastHdr(0u),
      mAsyncOutput(true),
      mJobValid(false),
      mJobRun(false),
      mJobDone(false),
      mWorkerExit(false) {
    ALOGD("C2FFMPEGVideoDecodeComponent: mediaType = %s", componentInfo->mediaType);
}

C2FFMPEGVideoDecodeComponent::~C2FFMPEGVideoDecodeComponent() {
    {
        std::lock_guard<std::mutex> lk(mJobMutex);
        mWorkerExit = true;
    }
    mJobCv.notify_all();
    if (mWorker.joinable()) {
        mWorker.join();
    }
    ALOGD("~C2FFMPEGVideoDecodeComponent: mCtx = %p", mCtx);
    onRelease();
}

c2_status_t C2FFMPEGVideoDecodeComponent::initDecoder() {
    if (! mFFMPEGInitialized) {
        if (initFFmpeg() != C2_OK) {
            ALOGE("initDecoder: FFMPEG initialization failed.");
            return C2_NO_INIT;
        }
        mFFMPEGInitialized = true;
    }

    mCtx = avcodec_alloc_context3(NULL);
    if (! mCtx) {
        ALOGE("initDecoder: avcodec_alloc_context failed.");
        return C2_NO_MEMORY;
    }

    C2StreamPictureSizeInfo::output size(0u, 320, 240);
    c2_status_t err = mIntf->query({ &size }, {}, C2_DONT_BLOCK, nullptr);
    if (err != C2_OK) {
        ALOGE("initDecoder: cannot query picture size, err = %d", err);
    }

    mCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    mCtx->codec_id = mCodecID;
    mCtx->extradata_size = 0;
    mCtx->extradata = NULL;
    mCtx->width = size.width;
    mCtx->height = size.height;

    ALOGD("initDecoder: %p [%s], %d x %d, %s",
          mCtx, avcodec_get_name(mCtx->codec_id), size.width, size.height, mInfo->mediaType);

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::openDecoder() {
    if (mCodecAlreadyOpened) {
        return C2_OK;
    }

    // Can't change extradata after opening the decoder.
#if DEBUG_EXTRADATA
    ALOGD("openDecoder: extradata_size = %d", mCtx->extradata_size);
#endif
    mExtradataReady = true;

    // Find decoder again as codec_id may have changed.
    if (mCtx->codec_id == AV_CODEC_ID_H264 &&
            base::GetBoolProperty("persist.vendor.ffmpeg_codec2.v4l2.h264", false)) {
        mCtx->codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    } else {
        mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
    }

    if (! mCtx->codec) {
        ALOGE("openDecoder: ffmpeg video decoder failed to find codec %d", mCtx->codec_id);
        return C2_NOT_FOUND;
    }

    // Configure decoder.
    mCtx->workaround_bugs   = 1;
    mCtx->idct_algo         = 0;
    mCtx->skip_frame        = AVDISCARD_DEFAULT;
    mCtx->skip_idct         = AVDISCARD_DEFAULT;
    mCtx->skip_loop_filter  = AVDISCARD_DEFAULT;
    mCtx->error_concealment = 3;
    mCtx->thread_count      = base::GetIntProperty("debug.ffmpeg_codec2.threads", 0);

    if (base::GetBoolProperty("debug.ffmpeg_codec2.fast", false)) {
        mCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    }

    ffmpeg_hwaccel_init(mCtx);

    ALOGD("openDecoder: opening ffmpeg decoder(%s): threads = %d, hw = %s",
          avcodec_get_name(mCtx->codec_id), mCtx->thread_count, mCtx->hw_device_ctx ? "yes" : "no");

    int err = avcodec_open2(mCtx, mCtx->codec, NULL);
    if (err < 0) {
        ALOGE("openDecoder: ffmpeg video decoder failed to initialize. (%s)", av_err2str(err));
        return C2_NO_INIT;
    }
    mCodecAlreadyOpened = true;

    ALOGD("openDecoder: open ffmpeg video decoder(%s) profile(%s) success, caps = %08x",
          avcodec_get_name(mCtx->codec_id),
          avcodec_profile_name(mCtx->codec_id, mCtx->profile),
          mCtx->codec->capabilities);

    mFrame = av_frame_alloc();
    if (! mFrame) {
        ALOGE("openDecoder: oom for video frame");
        return C2_NO_MEMORY;
    }

    initOutputPolicy();

    return C2_OK;
}

// Decide once per decoder open whether 10-bit output may be used at all.
void C2FFMPEGVideoDecodeComponent::initOutputPolicy() {
    mP010Mode = 0;
    if (!mIntf->isHevc()) {
        return;
    }

    const std::string mode = base::GetProperty("persist.vendor.ffmpeg_codec2.p010", "auto");
    mP010Mode = mode == "off" ? 0 : (mode == "always" ? 2 : 1);
    mDirectNv12 = base::GetBoolProperty("persist.vendor.ffmpeg_codec2.direct_nv12", true);
    mAsyncOutput = base::GetBoolProperty("persist.vendor.ffmpeg_codec2.async_output", true);

    // Only take the parameter value as the client's choice if it is not a
    // value this component configured itself on a previous run.
    const uint32_t fmt = mIntf->getPixelFormatLocked();
    if (fmt != mHalPixelFormat || fmt == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
        mClientPixelFormat = fmt;
    }

    // Returns P010 only if gralloc can allocate it (AHardwareBuffer_isSupported);
    // otherwise RGBA_1010102 or YV12, which this component cannot produce.
    // NOTE: minigbm's gbm_mesa backend (rpi4) has no DRM_FORMAT_P010
    // combination, so without a minigbm change this is false and the
    // component keeps the 8-bit path instead of failing every frame.
    mP010Supported = getHalPixelFormatForBitDepth10(false) == HAL_PIXEL_FORMAT_YCBCR_P010;

    ALOGD("initOutputPolicy: p010 mode %d, client pixel format 0x%x, gralloc P010 %s",
          mP010Mode, mClientPixelFormat, mP010Supported ? "yes" : "no");
}

void C2FFMPEGVideoDecodeComponent::deInitDecoder() {
    ALOGD("%p deInitDecoder: %p", this, mCtx);
    dropJob();
    if (mCtx) {
        if (avcodec_is_open(mCtx)) {
            avcodec_flush_buffers(mCtx);
        }
        ffmpeg_hwaccel_deinit(mCtx);
        avcodec_free_context(&mCtx);
        mCodecAlreadyOpened = false;
    }
    if (mFrame) {
        av_frame_free(&mFrame);
        mFrame = NULL;
    }
    if (mPacket) {
        av_packet_free(&mPacket);
        mPacket = NULL;
    }
    if (mImgConvertCtx) {
        sws_freeContext(mImgConvertCtx);
        mImgConvertCtx = NULL;
    }
    mEOSSignalled = false;
    mExtradataReady = false;
    // mHalPixelFormat is intentionally kept: it mirrors the interface
    // parameter, which survives a reset. Resetting it to YV12 here while the
    // parameter still says P010 would make the next 8-bit frame skip the
    // pixel-format configUpdate.
    mOutput10Bit = false;
    mColorAspectsSent = false;
    mHdrSent = false;
    mPendingWorkQueue.clear();
}

c2_status_t C2FFMPEGVideoDecodeComponent::processCodecConfig(C2ReadView* inBuffer) {
    int orig_extradata_size = mCtx->extradata_size;
    int add_extradata_size = inBuffer->capacity();

#if DEBUG_EXTRADATA
    ALOGD("processCodecConfig: add = %u, current = %d", add_extradata_size, orig_extradata_size);
#endif
    if (! mExtradataReady) {
        mCtx->extradata_size += add_extradata_size;
        mCtx->extradata = (uint8_t *) realloc(mCtx->extradata, mCtx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (! mCtx->extradata) {
            ALOGE("processCodecConfig: ffmpeg video decoder failed to alloc extradata memory.");
            return C2_NO_MEMORY;
        }
        memcpy(mCtx->extradata + orig_extradata_size, inBuffer->data(), add_extradata_size);
        memset(mCtx->extradata + mCtx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    }
    else {
        ALOGW("processCodecConfig: decoder is already opened, ignoring...");
    }

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::sendInputBuffer(
        C2ReadView *inBuffer, int64_t timestamp) {
    if (!mPacket) {
        mPacket = av_packet_alloc();
        if (!mPacket) {
            ALOGE("sendInputBuffer: oom for video packet");
            return C2_NO_MEMORY;
        }
    }

    mPacket->data = inBuffer ? const_cast<uint8_t *>(inBuffer->data()) : NULL;
    mPacket->size = inBuffer ? inBuffer->capacity() : 0;
    mPacket->pts = timestamp;
    mPacket->dts = AV_NOPTS_VALUE;

    int err = avcodec_send_packet(mCtx, mPacket);
    av_packet_unref(mPacket);

    if (err < 0) {
        ALOGE("sendInputBuffer: failed to send data (%d) to decoder: %s (%08x)",
              inBuffer->capacity(), av_err2str(err), err);
        if (err == AVERROR(EAGAIN)) {
            // Frames must be read first, notify main decoding loop.
            ALOGD("sendInputBuffer: returning C2_BAD_STATE");
            return C2_BAD_STATE;
        }
        // Otherwise don't send error to client.
    }

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::receiveFrame(bool* hasPicture) {
    int err = avcodec_receive_frame(mCtx, mFrame);

    *hasPicture = false;
    if (err == 0) {
        // Decide before the HW transfer: the transfer format depends on it.
        // Colour properties are valid on the HW frame (ff_decode_frame_props
        // runs in ff_get_buffer for hwaccel frames too).
        mOutput10Bit = allow10BitOutput();
        mFrameIsHw = false;
        if (mDirectNv12 && !mOutput10Bit && mFrame->hw_frames_ctx) {
            // Keep the frame in decoder memory: it is converted straight into the
            // output buffer in getOutputBuffer(), which saves a full-frame pass.
            mFrameIsHw = true;
            err = 0;
        } else {
            err = ffmpeg_hwaccel_get_frame(mCtx, mFrame, mOutput10Bit);
        }
        if (err == 0) {
            *hasPicture = true;
        } else {
            ALOGE("receiveFrame: failed to receive frame from HW decoder err = %d", err);
            // Don't send error to client, skip frame!
        }
    } else if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
        ALOGE("receiveFrame: failed to receive frame from decoder err = %d", err);
        // Don't report error to client.
    }

    return C2_OK;
}

static C2Color::range_t toC2Range(enum AVColorRange r) {
    switch (r) {
        case AVCOL_RANGE_MPEG: return C2Color::RANGE_LIMITED;
        case AVCOL_RANGE_JPEG: return C2Color::RANGE_FULL;
        default:               return C2Color::RANGE_UNSPECIFIED;
    }
}

static C2Color::primaries_t toC2Primaries(enum AVColorPrimaries p) {
    switch (p) {
        case AVCOL_PRI_BT709:     return C2Color::PRIMARIES_BT709;
        case AVCOL_PRI_BT2020:    return C2Color::PRIMARIES_BT2020;
        case AVCOL_PRI_BT470M:    return C2Color::PRIMARIES_BT470_M;
        case AVCOL_PRI_BT470BG:   return C2Color::PRIMARIES_BT601_625;
        case AVCOL_PRI_SMPTE170M:
        case AVCOL_PRI_SMPTE240M: return C2Color::PRIMARIES_BT601_525;
        default:                  return C2Color::PRIMARIES_UNSPECIFIED;
    }
}

static C2Color::transfer_t toC2Transfer(enum AVColorTransferCharacteristic t) {
    switch (t) {
        case AVCOL_TRC_SMPTE2084:    return C2Color::TRANSFER_ST2084;
        case AVCOL_TRC_ARIB_STD_B67: return C2Color::TRANSFER_HLG;
        case AVCOL_TRC_BT709:
        case AVCOL_TRC_SMPTE170M:
        case AVCOL_TRC_BT2020_10:
        case AVCOL_TRC_BT2020_12:    return C2Color::TRANSFER_170M;
        case AVCOL_TRC_SMPTE240M:    return C2Color::TRANSFER_240M;
        case AVCOL_TRC_LINEAR:       return C2Color::TRANSFER_LINEAR;
        case AVCOL_TRC_IEC61966_2_1: return C2Color::TRANSFER_SRGB;
        default:                     return C2Color::TRANSFER_UNSPECIFIED;
    }
}

static C2Color::matrix_t toC2Matrix(enum AVColorSpace m) {
    switch (m) {
        case AVCOL_SPC_BT709:       return C2Color::MATRIX_BT709;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:   return C2Color::MATRIX_BT2020;
        case AVCOL_SPC_SMPTE240M:   return C2Color::MATRIX_240M;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:   return C2Color::MATRIX_BT601;
        default:                    return C2Color::MATRIX_UNSPECIFIED;
    }
}

// Colour aspects of the current frame: bitstream (VUI) values where
// specified, container/client defaults (KEY_COLOR_*) otherwise - same rule as
// AOSP C2SoftHevcDec's ColorAspectsSetter.
C2ColorAspectsStruct C2FFMPEGVideoDecodeComponent::currentColorAspects() const {
    const C2ColorAspectsStruct def = mIntf->getDefaultColorAspectsLocked();
    C2ColorAspectsStruct a(toC2Range(mFrame->color_range), toC2Primaries(mFrame->color_primaries),
                           toC2Transfer(mFrame->color_trc), toC2Matrix(mFrame->colorspace));
    if (a.range == C2Color::RANGE_UNSPECIFIED) a.range = def.range;
    if (a.primaries == C2Color::PRIMARIES_UNSPECIFIED) a.primaries = def.primaries;
    if (a.transfer == C2Color::TRANSFER_UNSPECIFIED) a.transfer = def.transfer;
    if (a.matrix == C2Color::MATRIX_UNSPECIFIED) a.matrix = def.matrix;
    return a;
}

// Whether a 10-bit HEVC frame may be kept at 10 bits (P010) instead of being
// truncated to 8 bits. Controlled by persist.vendor.ffmpeg_codec2.p010
// (read in initOutputPolicy):
//   "auto"   (default) only HDR (PQ / HLG) streams
//   "always" every Main 10 stream
//   "off"    never (previous behaviour)
// and only when the client accepts a 10-bit format (Surface output =
// IMPLEMENTATION_DEFINED, or an explicit P010 request, which always wins) and
// gralloc can allocate P010.
bool C2FFMPEGVideoDecodeComponent::allow10BitOutput() const {
    if (!mIntf->isHevc() || !mFrame || mP010Mode == 0 || !mP010Supported) {
        return false;
    }
    if (mClientPixelFormat == HAL_PIXEL_FORMAT_YCBCR_P010) {
        return true;
    }
    if (mClientPixelFormat != HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
        return false;
    }
    if (mP010Mode == 2) {
        return true;
    }
    const C2Color::transfer_t trc = currentColorAspects().transfer;
    if (trc != C2Color::TRANSFER_ST2084 && trc != C2Color::TRANSFER_HLG) {
        return false;
    }
    // 10-bit (P010) frames cannot be scanned out by the display hardware of the
    // Raspberry Pi 4 and are composed by the GPU, which is too slow for 4K: keep the
    // faster 8-bit path above this width (persist.vendor.ffmpeg_codec2.p010_max_width).
    static const int maxWidth = base::GetIntProperty("persist.vendor.ffmpeg_codec2.p010_max_width", 2560);
    return mFrame->width <= maxWidth;
}

// Report colour aspects and HDR static metadata of the current frame to the
// framework (only when they changed), so a Surface consumer can pick the right
// dataspace and the display can be switched to HDR.
void C2FFMPEGVideoDecodeComponent::updateStreamInfo(
        std::vector<std::unique_ptr<C2Param>>* configUpdate) {
    if (!mIntf->isHevc()) {
        return;
    }

    const C2ColorAspectsStruct cur = currentColorAspects();
    C2StreamColorAspectsInfo::output aspects(0u, cur.range, cur.primaries, cur.transfer, cur.matrix);
    if (!mColorAspectsSent ||
            aspects.range != mLastAspects.range || aspects.primaries != mLastAspects.primaries ||
            aspects.transfer != mLastAspects.transfer || aspects.matrix != mLastAspects.matrix) {
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        if (mIntf->config({ &aspects }, C2_MAY_BLOCK, &failures) == C2_OK) {
            configUpdate->push_back(C2Param::Copy(aspects));
            mLastAspects = aspects;
            mColorAspectsSent = true;
        } else {
            ALOGW("updateStreamInfo: colour aspects config failed");
        }
    }

    const AVFrameSideData* mdSd =
            av_frame_get_side_data(mFrame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    const AVFrameSideData* cllSd =
            av_frame_get_side_data(mFrame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    C2StreamHdrStaticInfo::output hdr(0u);
    bool haveHdr = false;
    if (mdSd) {
        const AVMasteringDisplayMetadata* md = (const AVMasteringDisplayMetadata*)mdSd->data;
        if (md->has_primaries) {
            hdr.mastering.red   = { (float)av_q2d(md->display_primaries[0][0]),
                                    (float)av_q2d(md->display_primaries[0][1]) };
            hdr.mastering.green = { (float)av_q2d(md->display_primaries[1][0]),
                                    (float)av_q2d(md->display_primaries[1][1]) };
            hdr.mastering.blue  = { (float)av_q2d(md->display_primaries[2][0]),
                                    (float)av_q2d(md->display_primaries[2][1]) };
            hdr.mastering.white = { (float)av_q2d(md->white_point[0]),
                                    (float)av_q2d(md->white_point[1]) };
            haveHdr = true;
        }
        if (md->has_luminance) {
            hdr.mastering.maxLuminance = (float)av_q2d(md->max_luminance);
            hdr.mastering.minLuminance = (float)av_q2d(md->min_luminance);
            haveHdr = true;
        }
    }
    if (cllSd) {
        const AVContentLightMetadata* cll = (const AVContentLightMetadata*)cllSd->data;
        hdr.maxCll = (float)cll->MaxCLL;
        hdr.maxFall = (float)cll->MaxFALL;
        haveHdr = true;
    }
    if (haveHdr && (!mHdrSent || memcmp(&hdr.mastering, &mLastHdr.mastering, sizeof(hdr.mastering)) != 0 ||
                    hdr.maxCll != mLastHdr.maxCll || hdr.maxFall != mLastHdr.maxFall)) {
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        if (mIntf->config({ &hdr }, C2_MAY_BLOCK, &failures) == C2_OK) {
            configUpdate->push_back(C2Param::Copy(hdr));
            mLastHdr = hdr;
            mHdrSent = true;
        } else {
            ALOGW("updateStreamInfo: HDR static info config failed");
        }
    }
}

c2_status_t C2FFMPEGVideoDecodeComponent::getOutputBuffer(C2GraphicView* outBuffer, bool p010) {
    uint8_t* data[4];
    int linesize[4];
    C2PlanarLayout layout = outBuffer->layout();
    struct SwsContext* currentImgConvertCtx = mImgConvertCtx;

    data[0] = outBuffer->data()[C2PlanarLayout::PLANE_Y];
    data[1] = outBuffer->data()[C2PlanarLayout::PLANE_U];
    data[2] = outBuffer->data()[C2PlanarLayout::PLANE_V];
    linesize[0] = layout.planes[C2PlanarLayout::PLANE_Y].rowInc;
    linesize[1] = layout.planes[C2PlanarLayout::PLANE_U].rowInc;
    linesize[2] = layout.planes[C2PlanarLayout::PLANE_V].rowInc;

    if (mFrameIsHw) {
        // NV12 layout: interleaved CbCr, V pointer one byte after U, 2 byte column step.
        const bool nv12 = data[1] && data[2] && data[2] == data[1] + 1 &&
                          layout.planes[C2PlanarLayout::PLANE_U].colInc == 2 &&
                          layout.planes[C2PlanarLayout::PLANE_V].colInc == 2;
        if (nv12) {
            if (ffmpeg_hwaccel_nv12_into(mFrame, data[0], linesize[0], data[1], linesize[1],
                                         mFrame->width, mFrame->height) == 0) {
                return C2_OK;
            }
            ALOGE("getOutputBuffer: direct NV12 conversion failed, disabling it");
            mDirectNv12 = false;
            return C2_CORRUPTED;
        }
        // Not an NV12 buffer: use the CPU frame path (planar output buffer).
        ALOGW("getOutputBuffer: output buffer is not NV12, using the copy path");
        mDirectNv12 = false;
        if (ffmpeg_hwaccel_get_frame(mCtx, mFrame, false) != 0) {
            return C2_CORRUPTED;
        }
        mFrameIsHw = false;
    }

    // P010: Y plane + interleaved CbCr plane (data[1] = PLANE_U start, the V
    // pointer is PLANE_U + 2 and is ignored by swscale), 10 bits MSB-aligned
    // in 16. Everything else as 8-bit YV12 (PLANE_U/PLANE_V pointers take
    // care of the YV12 Cr-before-Cb order).
    // NOTE: the decision is passed in, not derived from mFrame->format: a
    // software-decoded Main 10 frame is YUV420P10LE too and must still be
    // converted to 8 bits when the client did not accept 10-bit output.
    const AVPixelFormat dstFmt = p010 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_YUV420P;

    // Fast path for the common 8-bit case: NV12 from the HW decoder into the
    // YV12 output buffer, multithreaded (swscale needs ~13 ms on one thread
    // for a 4K frame).
    if (!p010 && mFrame->format == AV_PIX_FMT_NV12 && data[1] && data[2] &&
        ffmpeg_nv12_to_yuv420p(mFrame->data[0], mFrame->linesize[0],
                               mFrame->data[1], mFrame->linesize[1],
                               data[0], linesize[0], data[1], linesize[1], data[2], linesize[2],
                               mFrame->width, mFrame->height, 4) == 0) {
        return C2_OK;
    }

    // P010 to P010: a plain row copy. swscale is not needed (and was observed to
    // corrupt chroma samples of 1080p frames).
    if (p010 && mFrame->format == AV_PIX_FMT_P010LE && data[1] && mFrame->data[1]) {
        const size_t rowBytes = (size_t)mFrame->width * 2;
        for (int y = 0; y < mFrame->height; y++) {
            memcpy(data[0] + (size_t)y * linesize[0], mFrame->data[0] + (size_t)y * mFrame->linesize[0], rowBytes);
        }
        for (int y = 0; y < (mFrame->height + 1) / 2; y++) {
            memcpy(data[1] + (size_t)y * linesize[1], mFrame->data[1] + (size_t)y * mFrame->linesize[1], rowBytes);
        }
        return C2_OK;
    }

    mImgConvertCtx = sws_getCachedContext(currentImgConvertCtx,
           mFrame->width, mFrame->height, (AVPixelFormat)mFrame->format,
           mFrame->width, mFrame->height, dstFmt,
           SWS_BICUBIC, NULL, NULL, NULL);
    if (mImgConvertCtx && mImgConvertCtx != currentImgConvertCtx) {
        ALOGD("getOutputBuffer: created video converter - %s => %s",
              av_get_pix_fmt_name((AVPixelFormat)mFrame->format), av_get_pix_fmt_name(dstFmt));

    } else if (! mImgConvertCtx) {
        ALOGE("getOutputBuffer: cannot initialize the conversion context");
        return C2_NO_MEMORY;
    }

    sws_scale(mImgConvertCtx, mFrame->data, mFrame->linesize,
              0, mFrame->height, data, linesize);

    return C2_OK;
}

static void fillEmptyWork(const std::unique_ptr<C2Work>& work) {
    work->worklets.front()->output.flags =
        (C2FrameData::flags_t)(work->input.flags & C2FrameData::FLAG_END_OF_STREAM);
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
    work->result = C2_OK;
#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: drop idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif
}

static bool comparePendingWork(const PendingWork& w1, const PendingWork& w2) {
    return w1.second < w2.second;
}

void C2FFMPEGVideoDecodeComponent::pushPendingWork(const std::unique_ptr<C2Work>& work) {
    uint32_t outputDelay = mIntf->getOutputDelay();

    // A frame held by the output worker is still waiting for its work to be finished:
    // it is one more legitimately pending work, not a work that never produces a frame.
    const size_t pendingLimit = outputDelay + ((mAsyncOutput && mJobValid) ? 1u : 0u);

    if (mPendingWorkQueue.size() >= pendingLimit) {
        uint32_t newOutputDelay = outputDelay;
        std::vector<std::unique_ptr<C2Param>> configUpdate;

        switch (mCtx->codec_id) {
            case AV_CODEC_ID_HEVC:
            case AV_CODEC_ID_H264:
                // Increase output delay step-wise.
                if (outputDelay >= 18u) {
                    newOutputDelay = 34u;
                } else if (outputDelay >= 8u) {
                    newOutputDelay = 18u;
                } else {
                    newOutputDelay = 8u;
                }
                break;
            default:
                // Other codecs use constant output delay.
                break;
        }

        if (newOutputDelay != outputDelay) {
            C2PortActualDelayTuning::output delay(newOutputDelay);
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            int err;

            err = mIntf->config({ &delay }, C2_MAY_BLOCK, &failures);
            if (err == C2_OK) {
                ALOGD("WorkQueue: queue full, output delay set to %u", newOutputDelay);
                configUpdate.push_back(C2Param::Copy(delay));
            } else {
                ALOGE("WorkQueue: output delay update to %u failed err = %d",
                      newOutputDelay, err);
            }
        }

        auto fillEmptyWorkWithConfigUpdate = [&configUpdate](const std::unique_ptr<C2Work>& work) {
            fillEmptyWork(work);
            work->worklets.front()->output.configUpdate = std::move(configUpdate);
        };

        finish(mPendingWorkQueue.front().first, fillEmptyWorkWithConfigUpdate);
        mPendingWorkQueue.pop_front();
    }
#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: push idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif
    mPendingWorkQueue.push_back(PendingWork(work->input.ordinal.frameIndex.peeku(),
                                            work->input.ordinal.timestamp.peeku()));
    std::sort(mPendingWorkQueue.begin(), mPendingWorkQueue.end(), comparePendingWork);
}

void C2FFMPEGVideoDecodeComponent::popPendingWork(const std::unique_ptr<C2Work>& work) {
    uint64_t index = work->input.ordinal.frameIndex.peeku();
    auto it = std::find_if(mPendingWorkQueue.begin(), mPendingWorkQueue.end(),
                           [index](const PendingWork& pWork) { return index == pWork.first; });

#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: pop idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif

    if (it != mPendingWorkQueue.end()) {
        mPendingWorkQueue.erase(it);
    }
#if DEBUG_WORKQUEUE
    else {
        ALOGD("WorkQueue: pop work not found idx=%" PRIu64 ", ts=%" PRIu64,
              work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
    }
#endif
    prunePendingWorksUntil(work);
}

void C2FFMPEGVideoDecodeComponent::prunePendingWorksUntil(const std::unique_ptr<C2Work>& work) {
#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: prune until idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif
    // Drop all works with a PTS earlier than provided argument.
    while (mPendingWorkQueue.size() > 0 &&
           mPendingWorkQueue.front().second < work->input.ordinal.timestamp.peeku()) {
        finish(mPendingWorkQueue.front().first, fillEmptyWork);
        mPendingWorkQueue.pop_front();
    }
}

c2_status_t C2FFMPEGVideoDecodeComponent::onInit() {
    ALOGD("onInit");
    return initDecoder();
}

c2_status_t C2FFMPEGVideoDecodeComponent::onStop() {
    ALOGD("onStop");
    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::onReset() {
    ALOGD("onReset");
    deInitDecoder();
    initDecoder();
}

void C2FFMPEGVideoDecodeComponent::onRelease() {
    ALOGD("onRelease");
    deInitDecoder();
    if (mFFMPEGInitialized) {
        deInitFFmpeg();
        mFFMPEGInitialized = false;
    }
}

c2_status_t C2FFMPEGVideoDecodeComponent::onFlush_sm() {
    ALOGD("onFlush_sm");
    dropJob();
    if (mCtx && avcodec_is_open(mCtx)) {
        // Make sure that the next buffer output does not still
        // depend on fragments from the last one decoded.
        avcodec_flush_buffers(mCtx);
        mEOSSignalled = false;
    }
    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::outputFrame(
    const std::unique_ptr<C2Work>& work,
    const std::shared_ptr<C2BlockPool> &pool
) {
    c2_status_t err;
    std::vector<std::unique_ptr<C2Param>> configUpdate;

#if DEBUG_FRAMES
    ALOGD("outputFrame: pts=%" PRId64 " dts=%" PRId64 " ts=%" PRId64 " - %d x %d (%x)",
          mFrame->pts, mFrame->pkt_dts, mFrame->best_effort_timestamp, mFrame->width, mFrame->height, mFrame->format);
#endif

    if (mFrame->width != mIntf->getWidth() || mFrame->height != mIntf->getHeight()) {
        ALOGD("outputFrame: video params changed - %d x %d (%x)", mFrame->width, mFrame->height, mFrame->format);

        C2StreamPictureSizeInfo::output size(0u, mFrame->width, mFrame->height);
        std::vector<std::unique_ptr<C2SettingResult>> failures;

        err = mIntf->config({ &size }, C2_MAY_BLOCK, &failures);
        if (err == OK) {
            configUpdate.push_back(C2Param::Copy(size));
            mCtx->width = mFrame->width;
            mCtx->height = mFrame->height;
        } else {
            ALOGE("outputFrame: config update failed err = %d", err);
            return C2_CORRUPTED;
        }
    }

    // 10-bit output only if decided so in receiveFrame() AND the frame really
    // is 10-bit (8-bit HW frames come out as YUV420P, Main 10 HW frames as
    // P010LE or YUV420P10LE when mOutput10Bit, SW Main 10 frames always
    // YUV420P10LE). P010LE -> P010LE is a plain line copy in swscale.
    bool p010 = mOutput10Bit && (mFrame->format == AV_PIX_FMT_YUV420P10LE ||
                                 mFrame->format == AV_PIX_FMT_P010LE);

    std::shared_ptr<C2GraphicBlock> block;
    err = pool->fetchGraphicBlock(mFrame->width, mFrame->height,
                                  p010 ? HAL_PIXEL_FORMAT_YCBCR_P010
                                       : (mFrameIsHw ? HAL_PIXEL_FORMAT_YCBCR_420_888 : HAL_PIXEL_FORMAT_YV12),
                                  { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE }, &block);
    if (err != C2_OK && mFrameIsHw) {
        // Flexible buffer not available: use the copy path with plain YV12.
        ALOGW("outputFrame: YCBCR_420_888 graphic block allocation failed (err = %d), using YV12", err);
        mDirectNv12 = false;
        if (ffmpeg_hwaccel_get_frame(mCtx, mFrame, false) != 0) {
            return C2_CORRUPTED;
        }
        mFrameIsHw = false;
        err = pool->fetchGraphicBlock(mFrame->width, mFrame->height, HAL_PIXEL_FORMAT_YV12,
                                      { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE }, &block);
    }
    if (err != C2_OK && p010) {
        // Do not abort playback: disable P010 for this instance and convert
        // the (already 16-bit) frame down to 8 bits with swscale.
        ALOGW("outputFrame: P010 graphic block allocation failed (err = %d), "
              "falling back to 8-bit output", err);
        mP010Supported = false;
        p010 = false;
        err = pool->fetchGraphicBlock(mFrame->width, mFrame->height, HAL_PIXEL_FORMAT_YV12,
                                      { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE }, &block);
    }
    const uint32_t halFormat = p010 ? HAL_PIXEL_FORMAT_YCBCR_P010
                                    : (mFrameIsHw ? HAL_PIXEL_FORMAT_YCBCR_420_888 : HAL_PIXEL_FORMAT_YV12);

    if (err != C2_OK) {
        ALOGE("outputFrame: failed to fetch graphic block %d x %d (%x) err = %d",
              mFrame->width, mFrame->height, halFormat, err);
        return C2_CORRUPTED;
    }

    if (halFormat != mHalPixelFormat) {
        // Tell the framework the buffers are now (or no longer) 10-bit.
        C2StreamPixelFormatInfo::output pixelFormat(0u, halFormat);
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        err = mIntf->config({ &pixelFormat }, C2_MAY_BLOCK, &failures);
        if (err != C2_OK) {
            ALOGE("outputFrame: pixel format config update to %x failed err = %d", halFormat, err);
            return C2_CORRUPTED;
        }
        configUpdate.push_back(C2Param::Copy(pixelFormat));
        mHalPixelFormat = halFormat;
    }

    updateStreamInfo(&configUpdate);

    C2GraphicView wView = block->map().get();

    err = wView.error();
    if (err != C2_OK) {
        ALOGE("outputFrame: graphic view map failed err = %d", err);
        return C2_CORRUPTED;
    }

    if (mAsyncOutput && mFrameIsHw && !p010) {
        C2PlanarLayout lay = wView.layout();
        uint8_t* dy = wView.data()[C2PlanarLayout::PLANE_Y];
        uint8_t* du = wView.data()[C2PlanarLayout::PLANE_U];
        uint8_t* dv = wView.data()[C2PlanarLayout::PLANE_V];
        const bool nv12 = du && dv && dv == du + 1 &&
                          lay.planes[C2PlanarLayout::PLANE_U].colInc == 2 &&
                          lay.planes[C2PlanarLayout::PLANE_V].colInc == 2;
        if (nv12) {
            // Deliver the previous frame first (keeps the output order), then hand
            // this one to the worker.
            err = completeJob();
            if (err != C2_OK) {
                return err;
            }
            OutputJob& job = mJob;
            job.hw = av_frame_clone(mFrame);
            if (!job.hw) {
                return C2_NO_MEMORY;
            }
            job.view.reset(new C2GraphicView(wView));
            job.block = std::move(block);
            job.halFormat = halFormat;
            job.configUpdate = std::move(configUpdate);
            job.frameIndex = (uint64_t)mFrame->best_effort_timestamp;
            job.width = mFrame->width;
            job.height = mFrame->height;
            job.dstY = dy;
            job.strideY = lay.planes[C2PlanarLayout::PLANE_Y].rowInc;
            job.dstC = du;
            job.strideC = lay.planes[C2PlanarLayout::PLANE_U].rowInc;
            job.result = 0;
            submitJob();
            return C2_OK;
        }
    }

    err = getOutputBuffer(&wView, p010);
    if (err == C2_OK) {
        std::shared_ptr<C2Buffer> buffer = createGraphicBuffer(std::move(block), C2Rect(mFrame->width, mFrame->height));

        // Attach the format of *this* buffer (an immutable snapshot) instead
        // of the live interface parameter, which may differ (e.g. still
        // IMPLEMENTATION_DEFINED as configured by the client) and is
        // replaced under the interface lock by other threads.
        buffer->setInfo(std::make_shared<C2StreamPixelFormatInfo::output>(0u, halFormat));

        if (work && c2_cntr64_t(mFrame->best_effort_timestamp) == work->input.ordinal.frameIndex) {
            prunePendingWorksUntil(work);
            work->worklets.front()->output.configUpdate = std::move(configUpdate);
            work->worklets.front()->output.buffers.clear();
            work->worklets.front()->output.buffers.push_back(buffer);
            work->worklets.front()->output.ordinal = work->input.ordinal;
            work->workletsProcessed = 1u;
            work->result = C2_OK;
        } else {
            auto fillWork = [buffer, &configUpdate, this](const std::unique_ptr<C2Work>& work) {
                popPendingWork(work);
                work->worklets.front()->output.configUpdate = std::move(configUpdate);
                work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
                work->worklets.front()->output.buffers.clear();
                work->worklets.front()->output.buffers.push_back(buffer);
                work->worklets.front()->output.ordinal = work->input.ordinal;
                work->workletsProcessed = 1u;
                work->result = C2_OK;
#if DEBUG_FRAMES
                ALOGD("outputFrame: work(finish) idx=%" PRIu64 ", processed=%u, result=%d",
                      work->input.ordinal.frameIndex.peeku(), work->workletsProcessed, work->result);
#endif
            };

            finish(mFrame->best_effort_timestamp, fillWork);
        }
    } else {
        return err;
    }

    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::process(
    const std::unique_ptr<C2Work> &work,
    const std::shared_ptr<C2BlockPool> &pool
) {
    size_t inSize = 0u;
    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM);
    C2ReadView rView = mDummyReadView;
    bool hasInputBuffer = false;

    if (! work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = rView.capacity();
        hasInputBuffer = true;
    }

#if DEBUG_FRAMES
    ALOGD("process: input flags=%08x ts=%lu idx=%lu #buf=%lu[%lu] #conf=%lu #info=%lu",
          work->input.flags, work->input.ordinal.timestamp.peeku(), work->input.ordinal.frameIndex.peeku(),
          work->input.buffers.size(), inSize, work->input.configUpdate.size(), work->input.infoBuffers.size());
#endif

    if (mEOSSignalled) {
        ALOGE("process: ignoring work while EOS reached");
        work->workletsProcessed = 0u;
        work->result = C2_BAD_VALUE;
        return;
    }

    if (hasInputBuffer && rView.error()) {
        ALOGE("process: read view map failed err = %d", rView.error());
        work->workletsProcessed = 0u;
        work->result = rView.error();
        return;
    }

    // In all cases the work is marked as completed.
    //
    // There is not always a 1:1 mapping between input and output frames, in particular for
    // interlaced content. Keeping the corresponding worklets in the queue quickly fills it
    // in and stalls the decoder. But there's no obvious mechanism to determine, from
    // FFMPEG API, whether a given packet will produce an output frame and the worklet should
    // be kept around so it can be completed when the frame is produced.
    //
    // NOTE: This has an impact on the drain operation.

    work->result = C2_OK;
    work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
    work->workletsProcessed = 0u;

    if (inSize || (eos && mCodecAlreadyOpened)) {
        c2_status_t err = C2_OK;

        if (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
            work->workletsProcessed = 1u;
            work->result = processCodecConfig(&rView);
            return;
        }

        if (! mCodecAlreadyOpened) {
            err = openDecoder();
            if (err != C2_OK) {
                work->workletsProcessed = 1u;
                work->result = err;
                return;
            }
        }

        bool inputConsumed = false;
        bool outputAvailable = true;
        bool hasPicture = false;
#if DEBUG_FRAMES
        int outputFrameCount = 0;
#endif

        while (!inputConsumed || outputAvailable) {
            if (!inputConsumed) {
                err = sendInputBuffer(&rView, work->input.ordinal.frameIndex.peekll());
                if (err == C2_OK) {
                    inputConsumed = true;
                    outputAvailable = true;
                    work->input.buffers.clear();
                } else if (err != C2_BAD_STATE) {
                    work->workletsProcessed = 1u;
                    work->result = err;
                    return;
                }
            }

            if (outputAvailable) {
                hasPicture = false;
                err = receiveFrame(&hasPicture);
                if (err != C2_OK) {
                    work->workletsProcessed = 1u;
                    work->result = err;
                    return;
                }

                if (hasPicture) {
                    err = outputFrame(work, pool);
                    if (err != C2_OK) {
                        work->workletsProcessed = 1u;
                        work->result = err;
                        return;
                    }
#if DEBUG_FRAMES
                    else {
                        outputFrameCount++;
                    }
#endif
                }
                else {
#if DEBUG_FRAMES
                    if (!outputFrameCount) {
                        ALOGD("process: no frame");
                    }
#endif
                    outputAvailable = false;
                }
            }
        }
    }
#if DEBUG_FRAMES
    else {
        ALOGD("process: empty work");
    }
#endif

    if (eos) {
        completeJob();
        mEOSSignalled = true;
        work->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
        work->workletsProcessed = 1u;
    }

    if (work->workletsProcessed == 0u) {
        pushPendingWork(work);
    }

#if DEBUG_FRAMES
    ALOGD("process: work(end) idx=%" PRIu64 ", processed=%u, result=%d",
          work->input.ordinal.frameIndex.peeku(), work->workletsProcessed, work->result);
#endif
}

c2_status_t C2FFMPEGVideoDecodeComponent::drain(
    uint32_t drainMode,
    const std::shared_ptr<C2BlockPool>& pool
) {
    ALOGD("drain: mode = %u", drainMode);

    if (drainMode == NO_DRAIN) {
        ALOGW("drain: NO_DRAIN is no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        ALOGW("drain: DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }
    if (! mCodecAlreadyOpened) {
        ALOGW("drain: codec not opened yet");
        return C2_OK;
    }

    bool hasPicture = false;
    c2_status_t err = C2_OK;

    err = sendInputBuffer(NULL, 0);
    while (err == C2_OK) {
        hasPicture = false;
        err = receiveFrame(&hasPicture);
        if (hasPicture) {
            // Ignore errors at this point, just drain the decoder.
            outputFrame(nullptr, pool);
        } else {
            err = C2_NOT_FOUND;
        }
    }

    completeJob();

    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::workerLoop() {
    // The conversion is memory bandwidth bound and, with four threads, saturates the
    // CPU: audio and UI threads (normal priority) then miss their deadlines. Run the
    // worker, and the helper threads it creates (they inherit the priority), niced.
    setpriority(PRIO_PROCESS, gettid(), base::GetIntProperty("persist.vendor.ffmpeg_codec2.worker_nice", 10));
    std::unique_lock<std::mutex> lk(mJobMutex);
    for (;;) {
        mJobCv.wait(lk, [this] { return mJobRun || mWorkerExit; });
        if (mWorkerExit) {
            return;
        }
        OutputJob& job = mJob;
        lk.unlock();
        ffmpeg_hwaccel_set_threads(base::GetIntProperty("persist.vendor.ffmpeg_codec2.sand_threads", 4));
        const int r = ffmpeg_hwaccel_nv12_into(job.hw, job.dstY, job.strideY, job.dstC, job.strideC,
                                               job.width, job.height);
        lk.lock();
        job.result = r;
        mJobRun = false;
        mJobDone = true;
        mJobCv.notify_all();
    }
}

void C2FFMPEGVideoDecodeComponent::submitJob() {
    std::lock_guard<std::mutex> lk(mJobMutex);
    if (!mWorker.joinable()) {
        mWorker = std::thread([this] { workerLoop(); });
    }
    mJobValid = true;
    mJobRun = true;
    mJobDone = false;
    mJobCv.notify_all();
}

c2_status_t C2FFMPEGVideoDecodeComponent::completeJob() {
    std::unique_lock<std::mutex> lk(mJobMutex);
    if (!mJobValid) {
        return C2_OK;
    }
    mJobCv.wait(lk, [this] { return mJobDone; });
    mJobValid = false;
    mJobDone = false;
    OutputJob job = std::move(mJob);
    mJob = OutputJob();
    lk.unlock();

    av_frame_free(&job.hw);

    if (job.result != 0) {
        ALOGE("completeJob: conversion failed (%d), dropping the frame", job.result);
        mDirectNv12 = false;
        finish(job.frameIndex, [this](const std::unique_ptr<C2Work>& work) {
            popPendingWork(work);
            fillEmptyWork(work);
        });
        return C2_OK;
    }

    std::shared_ptr<C2Buffer> buffer =
        createGraphicBuffer(std::move(job.block), C2Rect(job.width, job.height));
    buffer->setInfo(std::make_shared<C2StreamPixelFormatInfo::output>(0u, job.halFormat));
    job.view.reset();

    auto fillWork = [&buffer, &job, this](const std::unique_ptr<C2Work>& work) {
        popPendingWork(work);
        work->worklets.front()->output.configUpdate = std::move(job.configUpdate);
        work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
        work->result = C2_OK;
    };
    finish(job.frameIndex, fillWork);
    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::dropJob() {
    std::unique_lock<std::mutex> lk(mJobMutex);
    if (!mJobValid) {
        return;
    }
    mJobCv.wait(lk, [this] { return mJobDone; });
    mJobValid = false;
    mJobDone = false;
    OutputJob job = std::move(mJob);
    mJob = OutputJob();
    lk.unlock();
    av_frame_free(&job.hw);
}

} // namespace android
