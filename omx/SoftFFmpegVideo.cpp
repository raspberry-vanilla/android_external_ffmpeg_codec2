/*
 * Copyright 2012 Michael Chen <omxcodec@gmail.com>
 * Copyright 2015 The CyanogenMod Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "SoftFFmpegVideo"
#include <utils/Log.h>

#include "SoftFFmpegVideo.h"
#include "FFmpegComponents.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaDefs.h>

#define DEBUG_PKT 0
#define DEBUG_FRM 0

static int decoder_reorder_pts = -1;

namespace android {

static const CodecProfileLevel kM4VProfileLevels[] = {
    { OMX_VIDEO_MPEG4ProfileSimple, OMX_VIDEO_MPEG4Level5 },
    { OMX_VIDEO_MPEG4ProfileAdvancedSimple, OMX_VIDEO_MPEG4Level5 },
};

SoftFFmpegVideo::SoftFFmpegVideo(
        const char *name,
        const char *componentRole,
        OMX_VIDEO_CODINGTYPE codingType,
        const CodecProfileLevel *profileLevels,
        size_t numProfileLevels,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component,
        enum AVCodecID codecID)
    : SoftVideoDecoderOMXComponent(name, componentRole, codingType,
            profileLevels, numProfileLevels, 352, 288, callbacks, appData, component),
      mCodingType(codingType),
      mFFmpegAlreadyInited(false),
      mCodecAlreadyOpened(false),
      mCtx(NULL),
      mImgConvertCtx(NULL),
      mFrame(NULL),
      mEOSStatus(INPUT_DATA_AVAILABLE),
      mExtradataReady(false),
      mIgnoreExtradata(false),
      mStride(320),
      mSignalledError(false) {

    ALOGD("SoftFFmpegVideo component: %s codingType=%d appData: %p", name, codingType, appData);

    initPorts(
            kNumInputBuffers,
            1024 * 1024 /* inputBufferSize */,
            kNumOutputBuffers,
            name);

    CHECK_EQ(initDecoder(codecID), (status_t)OK);
}

SoftFFmpegVideo::~SoftFFmpegVideo() {
    ALOGV("~SoftFFmpegVideo");
    deInitDecoder();
    if (mFFmpegAlreadyInited) {
        deInitFFmpeg();
    }
}

void SoftFFmpegVideo::setDefaultCtx(AVCodecContext *avctx, const AVCodec *codec) {
    int fast = 1;

    avctx->workaround_bugs   = 1;
    avctx->lowres            = 0;
    if(avctx->lowres > codec->max_lowres){
        ALOGW("The maximum value for lowres supported by the decoder is %d",
                codec->max_lowres);
        avctx->lowres= codec->max_lowres;
    }
    avctx->idct_algo         = 0;
    avctx->skip_frame        = AVDISCARD_DEFAULT;
    avctx->skip_idct         = AVDISCARD_DEFAULT;
    avctx->skip_loop_filter  = AVDISCARD_ALL;
    avctx->error_concealment = 3;
    avctx->thread_count      = 0;

    if(avctx->lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
    if (fast)   avctx->flags2 |= CODEC_FLAG2_FAST;
    if(codec->capabilities & CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
}

status_t SoftFFmpegVideo::initDecoder(enum AVCodecID codecID) {
    status_t status;

    status = initFFmpeg();
    if (status != OK) {
        return NO_INIT;
    }
    mFFmpegAlreadyInited = true;

    mCtx = avcodec_alloc_context3(NULL);
    if (!mCtx)
    {
        ALOGE("avcodec_alloc_context failed.");
        return NO_MEMORY;
    }

    mCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    mCtx->codec_id = codecID;
    mCtx->extradata_size = 0;
    mCtx->extradata = NULL;
    mCtx->width = mWidth;
    mCtx->height = mHeight;
    return OK;
}

void SoftFFmpegVideo::deInitDecoder() {
    if (mCtx) {
        if (avcodec_is_open(mCtx)) {
            avcodec_flush_buffers(mCtx);
        }
        if (mCtx->extradata) {
            av_free(mCtx->extradata);
            mCtx->extradata = NULL;
            mCtx->extradata_size = 0;
        }
        if (mCodecAlreadyOpened) {
            avcodec_close(mCtx);
            av_free(mCtx);
            mCtx = NULL;
        }
    }
    if (mFrame) {
        av_freep(&mFrame);
        mFrame = NULL;
    }
    if (mImgConvertCtx) {
        sws_freeContext(mImgConvertCtx);
        mImgConvertCtx = NULL;
    }
}

OMX_ERRORTYPE SoftFFmpegVideo::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    //ALOGV("internalGetParameter index:0x%x", index);
    switch (index) {
        case OMX_IndexParamVideoWmv:
        {
            OMX_VIDEO_PARAM_WMVTYPE *profile =
                (OMX_VIDEO_PARAM_WMVTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eFormat = OMX_VIDEO_WMVFormatUnused;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoRv:
        {
            OMX_VIDEO_PARAM_RVTYPE *profile =
                (OMX_VIDEO_PARAM_RVTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eFormat = OMX_VIDEO_RVFormatUnused;

            return OMX_ErrorNone;
        }

        default:
        {
            if (index != (OMX_INDEXTYPE)OMX_IndexParamVideoFFmpeg) {
                return SoftVideoDecoderOMXComponent::internalGetParameter(index, params);
            }

            OMX_VIDEO_PARAM_FFMPEGTYPE *profile =
                (OMX_VIDEO_PARAM_FFMPEGTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eCodecId = AV_CODEC_ID_NONE;
            profile->nWidth   = 0;
            profile->nHeight  = 0;

            return OMX_ErrorNone;
        }
    }
}

OMX_ERRORTYPE SoftFFmpegVideo::isRoleSupported(
                const OMX_PARAM_COMPONENTROLETYPE *roleParams) {
    for (size_t i = 0;
         i < sizeof(kVideoComponents) / sizeof(kVideoComponents[0]);
         ++i) {
        if (strncmp((const char *)roleParams->cRole,
                kVideoComponents[i].mRole, OMX_MAX_STRINGNAME_SIZE - 1) == 0) {
            return OMX_ErrorNone;
        }
    }
    ALOGE("unsupported role: %s", (const char *)roleParams->cRole);
    return OMX_ErrorUndefined;
}

OMX_ERRORTYPE SoftFFmpegVideo::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    //ALOGV("internalSetParameter index:0x%x", index);
    switch (index) {
        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams =
                (const OMX_PARAM_COMPONENTROLETYPE *)params;
            return isRoleSupported(roleParams);
        }

        case OMX_IndexParamPortDefinition:
        {
            OMX_PARAM_PORTDEFINITIONTYPE *newParams =
                (OMX_PARAM_PORTDEFINITIONTYPE *)params;
            OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &newParams->format.video;
            OMX_PARAM_PORTDEFINITIONTYPE *def = &editPortInfo(newParams->nPortIndex)->mDef;

            uint32_t oldWidth = def->format.video.nFrameWidth;
            uint32_t oldHeight = def->format.video.nFrameHeight;
            uint32_t newWidth = video_def->nFrameWidth;
            uint32_t newHeight = video_def->nFrameHeight;
            if (newWidth != oldWidth || newHeight != oldHeight) {
                bool outputPort = (newParams->nPortIndex == kOutputPortIndex);
                if (outputPort) {
                    ALOGV("OMX_IndexParamPortDefinition (output) width=%d height=%d", newWidth, newHeight);

                    // only update (essentially crop) if size changes
                    mWidth = newWidth;
                    mHeight = newHeight;

                    updatePortDefinitions(true, true);
                    // reset buffer size based on frame size
                    newParams->nBufferSize = def->nBufferSize;
                } else {
                    // For input port, we only set nFrameWidth and nFrameHeight. Buffer size
                    // is updated when configuring the output port using the max-frame-size,
                    // though client can still request a larger size.
                    ALOGV("OMX_IndexParamPortDefinition (input) width=%d height=%d", newWidth, newHeight);
                    def->format.video.nFrameWidth = newWidth;
                    def->format.video.nFrameHeight = newHeight;
                    mCtx->width = newWidth;
                    mCtx->height = newHeight;
                }
            }
            return SoftVideoDecoderOMXComponent::internalSetParameter(index, params);
        }

        case OMX_IndexParamVideoWmv:
        {
            OMX_VIDEO_PARAM_WMVTYPE *profile =
                (OMX_VIDEO_PARAM_WMVTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (profile->eFormat == OMX_VIDEO_WMVFormat7) {
                mCtx->codec_id = AV_CODEC_ID_WMV1;
            } else if (profile->eFormat == OMX_VIDEO_WMVFormat8) {
                mCtx->codec_id = AV_CODEC_ID_WMV2;
            } else if (profile->eFormat == OMX_VIDEO_WMVFormat9) {
                mCtx->codec_id = AV_CODEC_ID_WMV3;
            } else {
                mCtx->codec_id = AV_CODEC_ID_VC1;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoRv:
        {
            OMX_VIDEO_PARAM_RVTYPE *profile =
                (OMX_VIDEO_PARAM_RVTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (profile->eFormat == OMX_VIDEO_RVFormatG2) {
                mCtx->codec_id = AV_CODEC_ID_RV20;
            } else if (profile->eFormat == OMX_VIDEO_RVFormat8) {
                mCtx->codec_id = AV_CODEC_ID_RV30;
            } else if (profile->eFormat == OMX_VIDEO_RVFormat9) {
                mCtx->codec_id = AV_CODEC_ID_RV40;
            } else {
                ALOGE("unsupported rv codec: 0x%x", profile->eFormat);
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        default:
        {
            if (index != (OMX_INDEXTYPE)OMX_IndexParamVideoFFmpeg) {
                return SoftVideoDecoderOMXComponent::internalSetParameter(index, params);
            }

            OMX_VIDEO_PARAM_FFMPEGTYPE *profile =
                (OMX_VIDEO_PARAM_FFMPEGTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->codec_id = (enum AVCodecID)profile->eCodecId;
            mCtx->width    = profile->nWidth;
            mCtx->height   = profile->nHeight;

            ALOGD("got OMX_IndexParamVideoFFmpeg, "
                "eCodecId:%d(%s), width:%u, height:%u",
                profile->eCodecId,
                avcodec_get_name(mCtx->codec_id),
                profile->nWidth,
                profile->nHeight);

            return OMX_ErrorNone;
        }
    }
}

int32_t SoftFFmpegVideo::handleExtradata() {
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    BufferInfo *inInfo = *inQueue.begin();
    OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;

    ALOGI("got extradata, ignore: %d, size: %u",
            mIgnoreExtradata, inHeader->nFilledLen);
    hexdump(inHeader->pBuffer + inHeader->nOffset, inHeader->nFilledLen);

    if (mIgnoreExtradata) {
        ALOGI("got extradata, size: %u, but ignore it", inHeader->nFilledLen);
    } else {
        if (!mExtradataReady) {
            //if (mMode == MODE_H264)
            //it is possible to receive multiple input buffer with OMX_BUFFERFLAG_CODECCONFIG flag.
            //for example, H264, the first input buffer is SPS, and another is PPS!
            int orig_extradata_size = mCtx->extradata_size;
            mCtx->extradata_size += inHeader->nFilledLen;
            mCtx->extradata = (uint8_t *)realloc(mCtx->extradata,
                    mCtx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
            if (!mCtx->extradata) {
                ALOGE("ffmpeg video decoder failed to alloc extradata memory.");
                return ERR_OOM;
            }

            memcpy(mCtx->extradata + orig_extradata_size,
                    inHeader->pBuffer + inHeader->nOffset,
                    inHeader->nFilledLen);
            memset(mCtx->extradata + mCtx->extradata_size, 0,
                    FF_INPUT_BUFFER_PADDING_SIZE);
        }
    }

    inQueue.erase(inQueue.begin());
    inInfo->mOwnedByUs = false;
    notifyEmptyBufferDone(inHeader);

    return ERR_OK;
}

int32_t SoftFFmpegVideo::openDecoder() {
    if (mCodecAlreadyOpened) {
        return ERR_OK;
    }

    if (!mExtradataReady) {
        ALOGI("extradata is ready, size: %d", mCtx->extradata_size);
        hexdump(mCtx->extradata, mCtx->extradata_size);
        mExtradataReady = true;
    }

    //find decoder again as codec_id may have changed
    mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
    if (!mCtx->codec) {
        ALOGE("ffmpeg video decoder failed to find codec");
        return ERR_CODEC_NOT_FOUND;
    }

    setDefaultCtx(mCtx, mCtx->codec);

    ALOGD("begin to open ffmpeg decoder(%s) now",
            avcodec_get_name(mCtx->codec_id));

    int err = avcodec_open2(mCtx, mCtx->codec, NULL);
    if (err < 0) {
        ALOGE("ffmpeg video decoder failed to initialize. (%s)", av_err2str(err));
        return ERR_DECODER_OPEN_FAILED;
    }
    mCodecAlreadyOpened = true;

    ALOGD("open ffmpeg video decoder(%s) success",
            avcodec_get_name(mCtx->codec_id));

    mFrame = av_frame_alloc();
    if (!mFrame) {
        ALOGE("oom for video frame");
        return ERR_OOM;
    }

    return ERR_OK;
}

void SoftFFmpegVideo::initPacket(AVPacket *pkt,
        OMX_BUFFERHEADERTYPE *inHeader) {
    memset(pkt, 0, sizeof(AVPacket));
    av_init_packet(pkt);

    if (inHeader) {
        pkt->data = (uint8_t *)inHeader->pBuffer + inHeader->nOffset;
        pkt->size = inHeader->nFilledLen;
        pkt->pts = inHeader->nTimeStamp;
        pkt->dts = inHeader->nTimeStamp;
    } else {
        pkt->data = NULL;
        pkt->size = 0;
        pkt->pts = AV_NOPTS_VALUE;
    }

#if DEBUG_PKT
    if (pkt->pts != AV_NOPTS_VALUE)
    {
        ALOGV("pkt size:%d, pts:%lld", pkt->size, pkt->pts);
    } else {
        ALOGV("pkt size:%d, pts:N/A", pkt->size);
    }
#endif
}

int32_t SoftFFmpegVideo::decodeVideo() {
    int len = 0, err = 0;
    int gotPic = false;
    int32_t ret = ERR_OK;
    bool is_flush = (mEOSStatus != INPUT_DATA_AVAILABLE);
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    BufferInfo *inInfo = NULL;
    OMX_BUFFERHEADERTYPE *inHeader = NULL;

    if (!is_flush) {
        inInfo = *inQueue.begin();
        CHECK(inInfo != NULL);
        inHeader = inInfo->mHeader;
    }

    AVPacket pkt;
    initPacket(&pkt, inHeader);

    av_frame_unref(mFrame);

    err = avcodec_decode_video2(mCtx, mFrame, &gotPic, &pkt);

    if (err < 0) {
        ALOGE("ffmpeg video decoder failed to decode frame. (%d)", err);
        //don't send error to OMXCodec, skip!
        ret = ERR_NO_FRM;
    } else {
        if (!gotPic) {
            ALOGI("ffmpeg video decoder failed to get frame.");
            //stop sending empty packets if the decoder is finished
            if (is_flush && mCtx->codec->capabilities & CODEC_CAP_DELAY) {
                ret = ERR_FLUSHED;
            } else {
                ret = ERR_NO_FRM;
            }
        } else {
            ret = ERR_OK;
        }
    }

    if (!is_flush) {
        inQueue.erase(inQueue.begin());
        inInfo->mOwnedByUs = false;
        notifyEmptyBufferDone(inHeader);
    }

    return ret;
}

int32_t SoftFFmpegVideo::drainOneOutputBuffer() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    BufferInfo *outInfo = *outQueue.begin();
    OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

    AVPicture pict;
    int64_t pts = AV_NOPTS_VALUE;
    uint8_t *dst = outHeader->pBuffer;

    uint32_t width = outputBufferWidth();
    uint32_t height = outputBufferHeight();

    memset(&pict, 0, sizeof(AVPicture));
    pict.data[0] = dst;
    pict.data[1] = dst + width * height;
    pict.data[2] = pict.data[1] + (width / 2  * height / 2);
    pict.linesize[0] = width;
    pict.linesize[1] = width / 2;
    pict.linesize[2] = width / 2;

    ALOGV("drainOneOutputBuffer: frame_width=%d frame_height=%d width=%d height=%d ctx_width=%d ctx_height=%d", mFrame->width, mFrame->height, width, height, mCtx->width, mCtx->height);

    int sws_flags = SWS_BICUBIC;
    mImgConvertCtx = sws_getCachedContext(mImgConvertCtx,
           mFrame->width, mFrame->height, (AVPixelFormat)mFrame->format, width, height,
           PIX_FMT_YUV420P, sws_flags, NULL, NULL, NULL);
    if (mImgConvertCtx == NULL) {
        ALOGE("Cannot initialize the conversion context");
        return ERR_SWS_FAILED;
    }
    sws_scale(mImgConvertCtx, mFrame->data, mFrame->linesize,
            0, height, pict.data, pict.linesize);

    outHeader->nOffset = 0;
    outHeader->nFilledLen = (outputBufferWidth() * outputBufferHeight() * 3) / 2;
    outHeader->nFlags = 0;
    if (mFrame->key_frame) {
        outHeader->nFlags |= OMX_BUFFERFLAG_SYNCFRAME;
    }

    //process timestamps
    if (decoder_reorder_pts == -1) {
        pts = av_frame_get_best_effort_timestamp(mFrame);
    } else if (decoder_reorder_pts) {
        pts = mFrame->pkt_pts;
    } else {
        pts = mFrame->pkt_dts;
    }

    if (pts == AV_NOPTS_VALUE) {
        pts = 0;
    }
    outHeader->nTimeStamp = pts; //FIXME pts is right???

#if DEBUG_FRM
    ALOGV("mFrame pkt_pts: %lld pkt_dts: %lld used %lld", mFrame->pkt_pts, mFrame->pkt_dts, pts);
#endif

    outQueue.erase(outQueue.begin());
    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);

    return ERR_OK;
}

void SoftFFmpegVideo::drainEOSOutputBuffer() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    BufferInfo *outInfo = *outQueue.begin();
    CHECK(outInfo != NULL);
    outQueue.erase(outQueue.begin());
    OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

    ALOGD("ffmpeg video decoder fill eos outbuf");

    outHeader->nTimeStamp = 0;
    outHeader->nFilledLen = 0;
    outHeader->nFlags = OMX_BUFFERFLAG_EOS;

    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);

    mEOSStatus = OUTPUT_FRAMES_FLUSHED;
}

void SoftFFmpegVideo::drainAllOutputBuffers() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
   if (!mCodecAlreadyOpened) {
        drainEOSOutputBuffer();
        mEOSStatus = OUTPUT_FRAMES_FLUSHED;
       return;
   }

    if(!(mCtx->codec->capabilities & CODEC_CAP_DELAY)) {
        drainEOSOutputBuffer();
        mEOSStatus = OUTPUT_FRAMES_FLUSHED;
        return;
    }

    while (!outQueue.empty()) {
        int32_t err = decodeVideo();
        if (err < ERR_OK) {
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            mSignalledError = true;
            return;
        } else if (err == ERR_FLUSHED) {
            drainEOSOutputBuffer();
            return;
        } else {
            CHECK_EQ(err, ERR_OK);
        }
        if (drainOneOutputBuffer() != ERR_OK) {
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            mSignalledError = true;
            return;
        }
    }
}

bool SoftFFmpegVideo::handlePortSettingsChange() {
    CropSettingsMode crop = kCropUnSet;
    uint32_t width  = outputBufferWidth();
    uint32_t height = outputBufferHeight();
    if (width != (uint32_t)mCtx->width || height != (uint32_t)mCtx->height) {
        crop = kCropSet;
        if (mCropWidth != width || mCropHeight != height) {
            mCropLeft = 0;
            mCropTop = 0;
            mCropWidth = width;
            mCropHeight = height;
            crop = kCropChanged;
        }
    }

    bool portWillReset = false;
    SoftVideoDecoderOMXComponent::handlePortSettingsChange(
            &portWillReset, mCtx->width, mCtx->height, crop);
    return portWillReset;
}

void SoftFFmpegVideo::onQueueFilled(OMX_U32 portIndex __unused) {
    if (mSignalledError || mOutputPortSettingsChange != NONE) {
        return;
    }

    if (mEOSStatus == OUTPUT_FRAMES_FLUSHED) {
        return;
    }

    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    while (((mEOSStatus != INPUT_DATA_AVAILABLE) || !inQueue.empty())
            && !outQueue.empty()) {

        if (mEOSStatus == INPUT_EOS_SEEN) {
            drainAllOutputBuffers();
            return;
        }

        BufferInfo *inInfo = *inQueue.begin();
        if (inInfo == NULL) {
            continue;
        }
        OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;
        if (inHeader == NULL) {
            continue;
        }

        if (inHeader->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
            ALOGD("ffmpeg got codecconfig buffer");
            if (handleExtradata() != ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
            }
            continue;
        }

        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            mEOSStatus = INPUT_EOS_SEEN;
        }

        if (!mCodecAlreadyOpened) {
            if (openDecoder() != ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }
        }

        int32_t err = decodeVideo();
        if (err < ERR_OK) {
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            mSignalledError = true;
            return;
        } else if (err == ERR_FLUSHED) {
            drainEOSOutputBuffer();
            return;
        } else if (err == ERR_NO_FRM) {
            continue;
        } else {
            CHECK_EQ(err, ERR_OK);
        }

        if (handlePortSettingsChange()) {
            ALOGV("PORT RESET w=%d h=%d", mCtx->width, mCtx->height);
            return;
        }

        if (drainOneOutputBuffer() != ERR_OK) {
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            mSignalledError = true;
            return;
        }
    }
}

void SoftFFmpegVideo::onPortFlushCompleted(OMX_U32 portIndex) {
    ALOGV("ffmpeg video decoder flush port(%u)", portIndex);
    if (portIndex == kInputPortIndex) {
        if (mCtx && avcodec_is_open(mCtx)) {
            //Make sure that the next buffer output does not still
            //depend on fragments from the last one decoded.
            avcodec_flush_buffers(mCtx);
        }
        mEOSStatus = INPUT_DATA_AVAILABLE;
    }
}

void SoftFFmpegVideo::onReset() {
    ALOGV("onReset()");
    SoftVideoDecoderOMXComponent::onReset();
    mSignalledError = false;
    mExtradataReady = false;
}

SoftOMXComponent* SoftFFmpegVideo::createSoftOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {

    OMX_VIDEO_CODINGTYPE codingType = OMX_VIDEO_CodingAutoDetect;
    char *componentRole = NULL;
    enum AVCodecID codecID = AV_CODEC_ID_NONE;

    for (size_t i = 0; i < kNumVideoComponents; ++i) {
        if (!strcasecmp(name, kVideoComponents[i].mName)) {
            componentRole = strdup(kVideoComponents[i].mRole);
            codingType = kVideoComponents[i].mVideoCodingType;
            codecID = kVideoComponents[i].mCodecID;
            break;
        }
    }

    if (componentRole == NULL) {
        TRESPASS();
    }

    if (!strcmp(name, "OMX.ffmpeg.mpeg4.decoder")) {
        return new SoftFFmpegVideo(name, componentRole, codingType,
                kM4VProfileLevels, ARRAY_SIZE(kM4VProfileLevels),
                callbacks, appData, component, codecID);
    }

    return new SoftFFmpegVideo(name, componentRole, codingType,
                NULL, 0,
                callbacks, appData, component, codecID);
}

}  // namespace android
