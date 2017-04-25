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

#ifndef SOFT_FFMPEGVIDEO_H_

#define SOFT_FFMPEGVIDEO_H_

#include "SoftVideoDecoderOMXComponent.h"

#include "utils/ffmpeg_utils.h"

namespace android {

struct SoftFFmpegVideo : public SoftVideoDecoderOMXComponent {
    SoftFFmpegVideo(const char *name,
            const char *componentRole,
            OMX_VIDEO_CODINGTYPE codingType,
            const CodecProfileLevel *profileLevels,
            size_t numProfileLevels,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component,
            enum AVCodecID codecID);

public:
    static SoftOMXComponent* createSoftOMXComponent(
            const char *name, const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData, OMX_COMPONENTTYPE **component);

protected:
    virtual ~SoftFFmpegVideo();

    virtual OMX_ERRORTYPE internalGetParameter(
            OMX_INDEXTYPE index, OMX_PTR params);

    virtual OMX_ERRORTYPE internalSetParameter(
            OMX_INDEXTYPE index, const OMX_PTR params);

    virtual void onQueueFilled(OMX_U32 portIndex);
    virtual void onPortFlushCompleted(OMX_U32 portIndex);
    virtual void onReset();

private:
    enum {
        kInputPortIndex   = 0,
        kOutputPortIndex  = 1,
        kNumInputBuffers  = 8,
        kNumOutputBuffers = 2,
    };

    enum EOSStatus {
        INPUT_DATA_AVAILABLE,
        INPUT_EOS_SEEN,
        OUTPUT_FRAMES_FLUSHED
    };

    enum {
        ERR_NO_FRM              = 2,
        ERR_FLUSHED             = 1,
        ERR_OK                  = 0,  //No errors
        ERR_OOM                 = -1, //Out of memmory
        ERR_CODEC_NOT_FOUND     = -2,
        ERR_DECODER_OPEN_FAILED = -2,
        ERR_SWS_FAILED          = -3,
    };

    OMX_VIDEO_CODINGTYPE mCodingType;
    bool mFFmpegAlreadyInited;
    bool mCodecAlreadyOpened;
    AVCodecContext *mCtx;
    struct SwsContext *mImgConvertCtx;
    AVFrame *mFrame;

    EOSStatus mEOSStatus;

    bool mExtradataReady;
    bool mIgnoreExtradata;
    int32_t mStride;
    int32_t mOutputWidth;
    int32_t mOutputHeight;

    bool mSignalledError;

    void     initInputFormat(uint32_t mode, OMX_PARAM_PORTDEFINITIONTYPE *def);
    void     getInputFormat(uint32_t mode, OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams);
    void     setDefaultCtx(AVCodecContext *avctx, const AVCodec *codec);
    OMX_ERRORTYPE isRoleSupported(const OMX_PARAM_COMPONENTROLETYPE *roleParams);

    status_t initDecoder(enum AVCodecID codecID);
    void     deInitDecoder();

    bool     isPortSettingChanged();

    int32_t  handleExtradata();
    int32_t  openDecoder();
    void     initPacket(AVPacket *pkt, OMX_BUFFERHEADERTYPE *inHeader);
    int32_t  decodeVideo();
    int32_t  preProcessVideoFrame(AVPicture *picture, void **bufp);
    int32_t  drainOneOutputBuffer();
    void     drainEOSOutputBuffer();
    void     drainAllOutputBuffers();
    bool     handlePortSettingsChange();

    DISALLOW_EVIL_CONSTRUCTORS(SoftFFmpegVideo);
};

}  // namespace android

#endif  // SOFT_FFMPEGVIDEO_H_
