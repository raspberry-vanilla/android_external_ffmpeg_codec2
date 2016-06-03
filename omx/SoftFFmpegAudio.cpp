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
#define LOG_TAG "SoftFFmpegAudio"
#include <utils/Log.h>
#include <cutils/properties.h>

#include "SoftFFmpegAudio.h"
#include "FFmpegComponents.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/OMXCodec.h>

#include <OMX_AudioExt.h>
#include <OMX_IndexExt.h>

#define DEBUG_PKT 0
#define DEBUG_FRM 0

namespace android {

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

int64_t *SoftFFmpegAudio::sAudioClock;

SoftFFmpegAudio::SoftFFmpegAudio(
        const char *name,
        const char *componentRole,
        OMX_AUDIO_CODINGTYPE codingType,
        enum AVCodecID codecID,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SimpleSoftOMXComponent(name, callbacks, appData, component),
      mRole(componentRole),
      mCodingType(codingType),
      mFFmpegAlreadyInited(false),
      mCodecAlreadyOpened(false),
      mExtradataReady(false),
      mIgnoreExtradata(false),
      mCtx(NULL),
      mSwrCtx(NULL),
      mFrame(NULL),
      mEOSStatus(INPUT_DATA_AVAILABLE),
      mSignalledError(false),
      mInputBufferSize(0),
      mResampledData(NULL),
      mResampledDataSize(0),
      mOutputPortSettingsChange(NONE),
      mReconfiguring(false) {

    setAudioClock(0);

    ALOGD("SoftFFmpegAudio component: %s mCodingType: %d",
            name, mCodingType);

    initPorts();
    CHECK_EQ(initDecoder(codecID), (status_t)OK);
}

SoftFFmpegAudio::~SoftFFmpegAudio() {
    ALOGV("~SoftFFmpegAudio");
    deInitDecoder();
    if (mFFmpegAlreadyInited) {
        deInitFFmpeg();
    }
}

void SoftFFmpegAudio::initPorts() {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    def.nPortIndex = 0;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = kNumInputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;

    if (mCodingType == (OMX_AUDIO_CODINGTYPE)OMX_AUDIO_CodingAPE) {
        def.nBufferSize = 1000000; // ape!
    } else if (mCodingType == (OMX_AUDIO_CODINGTYPE)OMX_AUDIO_CodingDTS) {
        def.nBufferSize = 1000000; // dts!
    } else {
        // max aggregated buffer size from nuplayer
        def.nBufferSize = 32 * 1024;
    }

    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainAudio;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 1;

    //def.format.audio.cMIMEType = const_cast<char *>("audio/raw");
    def.format.audio.pNativeRender = NULL;
    def.format.audio.bFlagErrorConcealment = OMX_FALSE;
    def.format.audio.eEncoding = mCodingType;

    addPort(def);

    def.nPortIndex = 1;
    def.eDir = OMX_DirOutput;
    def.nBufferCountMin = kNumOutputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = kOutputBufferSize;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainAudio;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 2;

    def.format.audio.cMIMEType = const_cast<char *>("audio/raw");
    def.format.audio.pNativeRender = NULL;
    def.format.audio.bFlagErrorConcealment = OMX_FALSE;
    def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;

    addPort(def);
}

void SoftFFmpegAudio::setDefaultCtx(AVCodecContext *avctx, const AVCodec *codec) {
    int fast = 0;

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
    avctx->skip_loop_filter  = AVDISCARD_DEFAULT;
    avctx->error_concealment = 3;

    avctx->flags |= CODEC_FLAG_BITEXACT;

    if(avctx->lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
    if (fast)   avctx->flags2 |= CODEC_FLAG2_FAST;
    if(codec->capabilities & CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
}

bool SoftFFmpegAudio::isConfigured() {
    return mCtx->channels > 0;
}

void SoftFFmpegAudio::resetCtx() {
    mCtx->channels = 2;
    mCtx->sample_rate = 44100;
    mCtx->bit_rate = 0;
    mCtx->sample_fmt = AV_SAMPLE_FMT_NONE;

    mAudioSrcChannels = mAudioTgtChannels = 2;
    mAudioSrcFreq = mAudioTgtFreq = 44100;
    mAudioSrcFmt = mAudioTgtFmt = AV_SAMPLE_FMT_NONE;
    mAudioSrcChannelLayout = mAudioTgtChannelLayout =
        av_get_default_channel_layout(mAudioSrcChannels);
}

void SoftFFmpegAudio::initVorbisHdr() {
    int32_t i = 0;
    for (i = 0; i < 3; i++) {
        mVorbisHeaderStart[i] = NULL;
        mVorbisHeaderLen[i] = 0;
    }
}

void SoftFFmpegAudio::deinitVorbisHdr() {
    int32_t i = 0;
    for (i = 0; i < 3; i++) {
        if (mVorbisHeaderLen[i] > 0) {
            av_free(mVorbisHeaderStart[i]);
            mVorbisHeaderStart[i] = NULL;
            mVorbisHeaderLen[i] = 0;
        }
    }
}

status_t SoftFFmpegAudio::initDecoder(enum AVCodecID codecID) {
    status_t status;

    status = initFFmpeg();
    if (status != OK) {
        return NO_INIT;
    }
    mFFmpegAlreadyInited = true;

    mCtx = avcodec_alloc_context3(NULL);
    if (!mCtx) {
        ALOGE("avcodec_alloc_context failed.");
        return NO_MEMORY;
    }

    mCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    mCtx->codec_id = codecID;

    //invalid ctx
    resetCtx();

    mCtx->extradata = NULL;
    mCtx->extradata_size = 0;

    initVorbisHdr();

    memset(mSilenceBuffer, 0, kOutputBufferSize);

    return OK;
}

void SoftFFmpegAudio::deInitDecoder() {
    if (mCtx) {
        if (!mCtx->extradata) {
            av_free(mCtx->extradata);
            mCtx->extradata = NULL;
            mCtx->extradata_size = 0;
        }

        deinitVorbisHdr();

        if (mCodecAlreadyOpened) {
            avcodec_close(mCtx);
            mCodecAlreadyOpened = false;
        }
        av_free(mCtx);
        mCtx = NULL;
    }
    if (mFrame) {
        av_frame_free(&mFrame);
        mFrame = NULL;
    }
    if (mSwrCtx) {
        swr_free(&mSwrCtx);
        mSwrCtx = NULL;
    }
}

OMX_ERRORTYPE SoftFFmpegAudio::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    ALOGV("internalGetParameter index:0x%x", index);
    switch ((int)index) {
        case OMX_IndexParamAudioPcm:
        {
            OMX_AUDIO_PARAM_PCMMODETYPE *profile =
                (OMX_AUDIO_PARAM_PCMMODETYPE *)params;

            if (profile->nPortIndex > kOutputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eNumData = OMX_NumericalDataSigned;
            profile->eEndian = OMX_EndianBig;
            profile->bInterleaved = OMX_TRUE;
            profile->ePCMMode = OMX_AUDIO_PCMModeLinear;

            if (isConfigured()) {
                AVSampleFormat packed = av_get_packed_sample_fmt(mAudioTgtFmt);
                if (packed == AV_SAMPLE_FMT_U8)
                    profile->nBitPerSample = 8;
                else if (packed == AV_SAMPLE_FMT_S16)
                    profile->nBitPerSample = 16;
                else if (packed == AV_SAMPLE_FMT_S32)
                    profile->nBitPerSample = 24;
                else
                    profile->nBitPerSample = av_get_bytes_per_sample(mAudioTgtFmt) * 8;
            } else {
                profile->nBitPerSample = 32;
            }

            if (getOMXChannelMapping(mAudioTgtChannels, profile->eChannelMapping) != OK) {
                return OMX_ErrorNone;
            }

            profile->nChannels = mAudioTgtChannels;
            profile->nSamplingRate = mAudioTgtFreq;

            //mCtx has been updated(adjustAudioParams)!
            ALOGV("get pcm params, nChannels:%u, nSamplingRate:%u, nBitsPerSample:%u",
                   profile->nChannels, profile->nSamplingRate, profile->nBitPerSample);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAac:
        {
            OMX_AUDIO_PARAM_AACPROFILETYPE *profile =
                (OMX_AUDIO_PARAM_AACPROFILETYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->nBitRate = 0;
            profile->nAudioBandWidth = 0;
            profile->nAACtools = 0;
            profile->nAACERtools = 0;
            profile->eAACProfile = OMX_AUDIO_AACObjectMain;
            profile->eAACStreamFormat = OMX_AUDIO_AACStreamFormatMP4FF;
            profile->eChannelMode = OMX_AUDIO_ChannelModeStereo;

            profile->nChannels = mCtx->channels;
            profile->nSampleRate = mCtx->sample_rate;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioMp3:
        {
            OMX_AUDIO_PARAM_MP3TYPE *profile =
                (OMX_AUDIO_PARAM_MP3TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->nBitRate = 0;
            profile->nAudioBandWidth = 0;
            profile->eChannelMode = OMX_AUDIO_ChannelModeStereo;
            profile->eFormat = OMX_AUDIO_MP3StreamFormatMP1Layer3;

            profile->nChannels = mCtx->channels;
            profile->nSampleRate = mCtx->sample_rate;

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioVorbis:
        {
            OMX_AUDIO_PARAM_VORBISTYPE *profile =
                (OMX_AUDIO_PARAM_VORBISTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->nBitRate = 0;
            profile->nMinBitRate = 0;
            profile->nMaxBitRate = 0;
            profile->nAudioBandWidth = 0;
            profile->nQuality = 3;
            profile->bManaged = OMX_FALSE;
            profile->bDownmix = OMX_FALSE;

            profile->nChannels = mCtx->channels;
            profile->nSampleRate = mCtx->sample_rate;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioWma:
        {
            OMX_AUDIO_PARAM_WMATYPE *profile =
                (OMX_AUDIO_PARAM_WMATYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eFormat = OMX_AUDIO_WMAFormatUnused;

            profile->nChannels = mCtx->channels;
            profile->nSamplingRate = mCtx->sample_rate;

            profile->nBlockAlign = mCtx->block_align;
            profile->nBitRate = mCtx->bit_rate;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioRa:
        {
            OMX_AUDIO_PARAM_RATYPE *profile =
                (OMX_AUDIO_PARAM_RATYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eFormat = OMX_AUDIO_RAFormatUnused;

            profile->nChannels = mCtx->channels;
            profile->nSamplingRate = mCtx->sample_rate;

            profile->nNumRegions = mCtx->block_align;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioFlac:
        {
            OMX_AUDIO_PARAM_FLACTYPE *profile =
                (OMX_AUDIO_PARAM_FLACTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->nChannels = mCtx->channels;
            profile->nSampleRate = mCtx->sample_rate;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioMp2:
        {
            OMX_AUDIO_PARAM_MP2TYPE *profile =
                (OMX_AUDIO_PARAM_MP2TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->nChannels = mCtx->channels;
            profile->nSampleRate = mCtx->sample_rate;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAndroidAc3:
        {
            OMX_AUDIO_PARAM_ANDROID_AC3TYPE *profile =
                (OMX_AUDIO_PARAM_ANDROID_AC3TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->nChannels = mCtx->channels;
            profile->nSampleRate = mCtx->sample_rate;

            return OMX_ErrorNone;
        }


        case OMX_IndexParamAudioAc3:
        {
            OMX_AUDIO_PARAM_AC3TYPE *profile =
                (OMX_AUDIO_PARAM_AC3TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->nChannels = mCtx->channels;
            profile->nSamplingRate = mCtx->sample_rate;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioApe:
        {
            OMX_AUDIO_PARAM_APETYPE *profile =
                (OMX_AUDIO_PARAM_APETYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->nChannels = mCtx->channels;
            profile->nSamplingRate = mCtx->sample_rate;

            profile->nBitsPerSample = mCtx->bits_per_coded_sample;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioDts:
        {
            OMX_AUDIO_PARAM_DTSTYPE *profile =
                (OMX_AUDIO_PARAM_DTSTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->nChannels = mCtx->channels;
            profile->nSamplingRate = mCtx->sample_rate;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioFFmpeg:
        {
            OMX_AUDIO_PARAM_FFMPEGTYPE *profile =
                (OMX_AUDIO_PARAM_FFMPEGTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eCodecId = mCtx->codec_id;
            profile->nBitRate = mCtx->bit_rate;
            profile->nBlockAlign = mCtx->block_align;

            profile->nBitsPerSample = mCtx->bits_per_raw_sample;
            profile->eSampleFormat = mCtx->sample_fmt;

            profile->nChannels = mCtx->channels;
            profile->nSampleRate = mCtx->sample_rate;

            return OMX_ErrorNone;
        }

        default:

            return SimpleSoftOMXComponent::internalGetParameter(index, params);
    }
}

OMX_ERRORTYPE SoftFFmpegAudio::isRoleSupported(
        const OMX_PARAM_COMPONENTROLETYPE *roleParams) {
    for (size_t i = 0; i < kNumAudioComponents; i++) {
        if (mCodingType == kAudioComponents[i].mAudioCodingType &&
            strncmp((const char *)roleParams->cRole,
                kAudioComponents[i].mRole, OMX_MAX_STRINGNAME_SIZE - 1) == 0) {
            return OMX_ErrorNone;
        }
    }
    ALOGE("unsupported role: %s", (const char *)roleParams->cRole);
    return OMX_ErrorUndefined;
}

void SoftFFmpegAudio::adjustAudioParams() {

    mReconfiguring = isConfigured();

    // let android audio mixer to downmix if there is no multichannel output
    // and use number of channels from the source file, useful for HDMI/offload output
    mAudioTgtChannels = mCtx->channels;

    mAudioTgtFreq = FFMIN(192000, FFMAX(8000, mCtx->sample_rate));

    mAudioTgtChannels = mCtx->channels;
    mAudioTgtFreq = mCtx->sample_rate;

    mAudioTgtChannelLayout = av_get_default_channel_layout(mAudioTgtChannels);

    ALOGV("adjustAudioParams: [channels=%d freq=%d fmt=%s]",
            mCtx->channels, mCtx->sample_rate, av_get_sample_fmt_name(mAudioTgtFmt));
}

OMX_ERRORTYPE SoftFFmpegAudio::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    //ALOGV("internalSetParameter index:0x%x", index);
    switch ((int)index) {
        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams =
                (const OMX_PARAM_COMPONENTROLETYPE *)params;
            return isRoleSupported(roleParams);
        }

        case OMX_IndexParamAudioPcm:
        {
            const OMX_AUDIO_PARAM_PCMMODETYPE *profile =
                (const OMX_AUDIO_PARAM_PCMMODETYPE *)params;

            if (profile->nPortIndex != kOutputPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (profile->nBitPerSample == 24) {
                mAudioTgtFmt = AV_SAMPLE_FMT_S32;
            } else if (profile->nBitPerSample == 32) {
                mAudioTgtFmt = AV_SAMPLE_FMT_FLT;
            } else if (profile->nBitPerSample == 8) {
                mAudioTgtFmt = AV_SAMPLE_FMT_U8;
            } else {
                mAudioTgtFmt = AV_SAMPLE_FMT_S16;
            }

            mAudioTgtFreq = profile->nSamplingRate;
            mAudioTgtChannels = profile->nChannels;

            ALOGV("set OMX_IndexParamAudioPcm, nChannels:%u, "
                    "nSampleRate:%u, nBitsPerSample:%u",
                profile->nChannels, profile->nSamplingRate,
                profile->nBitPerSample);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAac:
        {
            const OMX_AUDIO_PARAM_AACPROFILETYPE *profile =
                (const OMX_AUDIO_PARAM_AACPROFILETYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSampleRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioAac, nChannels:%u, nSampleRate:%u",
                profile->nChannels, profile->nSampleRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioMp3:
        {
            const OMX_AUDIO_PARAM_MP3TYPE *profile =
                (const OMX_AUDIO_PARAM_MP3TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSampleRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioMp3, nChannels:%u, nSampleRate:%u",
                profile->nChannels, profile->nSampleRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioVorbis:
        {
            const OMX_AUDIO_PARAM_VORBISTYPE *profile =
                (const OMX_AUDIO_PARAM_VORBISTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSampleRate;

            adjustAudioParams();

            ALOGD("set OMX_IndexParamAudioVorbis, "
                    "nChannels=%u, nSampleRate=%u, nBitRate=%u, "
                    "nMinBitRate=%u, nMaxBitRate=%u",
                profile->nChannels, profile->nSampleRate,
                profile->nBitRate, profile->nMinBitRate,
                profile->nMaxBitRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioWma:
        {
            OMX_AUDIO_PARAM_WMATYPE *profile =
                (OMX_AUDIO_PARAM_WMATYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (profile->eFormat == OMX_AUDIO_WMAFormat7) {
               mCtx->codec_id = AV_CODEC_ID_WMAV2;
            } else if (profile->eFormat == OMX_AUDIO_WMAFormat8) {
               mCtx->codec_id = AV_CODEC_ID_WMAPRO;
            } else if (profile->eFormat == OMX_AUDIO_WMAFormat9) {
               mCtx->codec_id = AV_CODEC_ID_WMALOSSLESS;
            } else {
                ALOGE("unsupported wma codec: 0x%x", profile->eFormat);
                return OMX_ErrorUndefined;
            }

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSamplingRate;

            // wmadec needs bitrate, block_align
            mCtx->bit_rate = profile->nBitRate;
            mCtx->block_align = profile->nBlockAlign;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioWma, nChannels:%u, "
                    "nSampleRate:%u, nBitRate:%u, nBlockAlign:%u",
                profile->nChannels, profile->nSamplingRate,
                profile->nBitRate, profile->nBlockAlign);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioRa:
        {
            OMX_AUDIO_PARAM_RATYPE *profile =
                (OMX_AUDIO_PARAM_RATYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSamplingRate;

            // FIXME, HACK!!!, I use the nNumRegions parameter pass blockAlign!!!
            // the cook audio codec need blockAlign!
            mCtx->block_align = profile->nNumRegions;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioRa, nChannels:%u, "
                    "nSampleRate:%u, nBlockAlign:%d",
                profile->nChannels, profile->nSamplingRate, mCtx->block_align);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioFlac:
        {
            OMX_AUDIO_PARAM_FLACTYPE *profile =
                (OMX_AUDIO_PARAM_FLACTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSampleRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioFlac, nChannels:%u, nSampleRate:%u ",
                profile->nChannels, profile->nSampleRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioMp2:
        {
            OMX_AUDIO_PARAM_MP2TYPE *profile =
                (OMX_AUDIO_PARAM_MP2TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSampleRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioMp2, nChannels:%u, nSampleRate:%u",
                profile->nChannels, profile->nSampleRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAc3:
        {
            OMX_AUDIO_PARAM_AC3TYPE *profile =
                (OMX_AUDIO_PARAM_AC3TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSamplingRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioAc3, nChannels:%u, nSampleRate:%u",
                profile->nChannels, profile->nSamplingRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAndroidAc3:
        {
            OMX_AUDIO_PARAM_ANDROID_AC3TYPE *profile =
                (OMX_AUDIO_PARAM_ANDROID_AC3TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSampleRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioAndroidAc3, nChannels:%u, nSampleRate:%u",
                profile->nChannels, profile->nSampleRate);

            return OMX_ErrorNone;
        }


        case OMX_IndexParamAudioApe:
        {
            OMX_AUDIO_PARAM_APETYPE *profile =
                (OMX_AUDIO_PARAM_APETYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSamplingRate;
            mCtx->bits_per_coded_sample = profile->nBitsPerSample;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioApe, nChannels:%u, "
                    "nSampleRate:%u, nBitsPerSample:%u",
                profile->nChannels, profile->nSamplingRate,
                profile->nBitsPerSample);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioDts:
        {
            OMX_AUDIO_PARAM_DTSTYPE *profile =
                (OMX_AUDIO_PARAM_DTSTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSamplingRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioDts, nChannels:%u, nSampleRate:%u",
                profile->nChannels, profile->nSamplingRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioFFmpeg:
        {
            OMX_AUDIO_PARAM_FFMPEGTYPE *profile =
                (OMX_AUDIO_PARAM_FFMPEGTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->codec_id = (enum AVCodecID)profile->eCodecId;
            mCtx->channels = profile->nChannels;
            mCtx->bit_rate = profile->nBitRate;
            mCtx->sample_rate = profile->nSampleRate;
            mCtx->block_align = profile->nBlockAlign;
            mCtx->bits_per_coded_sample = profile->nBitsPerSample;
            mCtx->sample_fmt = (AVSampleFormat)profile->eSampleFormat;

            adjustAudioParams();

            ALOGD("set OMX_IndexParamAudioFFmpeg, "
                "eCodecId:%d(%s), nChannels:%u, nBitRate:%u, "
                "nBitsPerSample:%u, nSampleRate:%u, "
                "nBlockAlign:%u, eSampleFormat:%u(%s)",
                profile->eCodecId, avcodec_get_name(mCtx->codec_id),
                profile->nChannels, profile->nBitRate,
                profile->nBitsPerSample, profile->nSampleRate,
                profile->nBlockAlign, profile->eSampleFormat,
                av_get_sample_fmt_name(mCtx->sample_fmt));
            return OMX_ErrorNone;
        }

        default:

            return SimpleSoftOMXComponent::internalSetParameter(index, params);
    }
}

int32_t SoftFFmpegAudio::handleVorbisExtradata(OMX_BUFFERHEADERTYPE *inHeader)
{
    uint8_t *p = inHeader->pBuffer + inHeader->nOffset;
    int len = inHeader->nFilledLen;
    int index = 0;

    if (p[0] == 1) {
        index = 0;
    } else if (p[0] == 3) {
        index = 1;
    } else if (p[0] == 5) {
        index = 2;
    } else {
        ALOGE("error vorbis codec config");
        return ERR_INVALID_PARAM;
    }

    mVorbisHeaderStart[index] = (uint8_t *)av_mallocz(len);
    if (!mVorbisHeaderStart[index]) {
        ALOGE("oom for vorbis extradata");
        return ERR_OOM;
    }
    memcpy(mVorbisHeaderStart[index], p, len);
    mVorbisHeaderLen[index] = inHeader->nFilledLen;

    return ERR_OK;
}

int32_t SoftFFmpegAudio::handleExtradata() {
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
            uint32_t ret = ERR_OK;
            if (mCtx->codec_id == AV_CODEC_ID_VORBIS) {
                if ((ret = handleVorbisExtradata(inHeader)) != ERR_OK) {
                    ALOGE("ffmpeg audio decoder failed to alloc vorbis extradata memory.");
                    return ret;
                }
            } else {
                int orig_extradata_size = mCtx->extradata_size;
                mCtx->extradata_size += inHeader->nFilledLen;
                mCtx->extradata = (uint8_t *)av_realloc(mCtx->extradata,
                    mCtx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
                if (!mCtx->extradata) {
                    ALOGE("ffmpeg audio decoder failed to alloc extradata memory.");
                    return ERR_OOM;
                }

                memcpy(mCtx->extradata + orig_extradata_size,
                    inHeader->pBuffer + inHeader->nOffset, inHeader->nFilledLen);
                memset(mCtx->extradata + mCtx->extradata_size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
            }
        }
    }

    inInfo->mOwnedByUs = false;
    inQueue.erase(inQueue.begin());
    inInfo = NULL;
    notifyEmptyBufferDone(inHeader);
    inHeader = NULL;

    return ERR_OK;
}

int32_t SoftFFmpegAudio::openDecoder() {
    if (mCodecAlreadyOpened) {
        return ERR_OK;
    }

    if (!mExtradataReady && !mIgnoreExtradata) {
        if (mCtx->codec_id == AV_CODEC_ID_VORBIS) {
            if (!setup_vorbis_extradata(&mCtx->extradata,
                        &mCtx->extradata_size,
                        (const uint8_t **)mVorbisHeaderStart,
                        mVorbisHeaderLen)) {
                return ERR_OOM;
            }
            deinitVorbisHdr();
        }
        ALOGI("extradata is ready, size: %d", mCtx->extradata_size);
        hexdump(mCtx->extradata, mCtx->extradata_size);
        mExtradataReady = true;
    }

    //find decoder
    mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
    if (!mCtx->codec) {
        ALOGE("ffmpeg audio decoder failed to find codec");
        return ERR_CODEC_NOT_FOUND;
    }

    setDefaultCtx(mCtx, mCtx->codec);

    ALOGD("begin to open ffmpeg audio decoder(%s), mCtx sample_rate: %d, channels: %d",
           avcodec_get_name(mCtx->codec_id),
           mCtx->sample_rate, mCtx->channels);

    int err = avcodec_open2(mCtx, mCtx->codec, NULL);
    if (err < 0) {
        ALOGE("ffmpeg audio decoder failed to initialize.(%s)", av_err2str(err));
        return ERR_DECODER_OPEN_FAILED;
    }
    mCodecAlreadyOpened = true;

    ALOGD("open ffmpeg audio decoder(%s) success, mCtx sample_rate: %d, "
            "channels: %d, sample_fmt: %s, bits_per_coded_sample: %d, bits_per_raw_sample: %d",
            avcodec_get_name(mCtx->codec_id),
            mCtx->sample_rate, mCtx->channels,
            av_get_sample_fmt_name(mCtx->sample_fmt),
            mCtx->bits_per_coded_sample, mCtx->bits_per_raw_sample);

    mFrame = av_frame_alloc();
    if (!mFrame) {
        ALOGE("oom for video frame");
        return ERR_OOM;
    }

    mAudioSrcFmt = mCtx->sample_fmt;
    mAudioSrcChannels = mCtx->channels;
    mAudioSrcFreq = mCtx->sample_rate;
    mAudioSrcChannelLayout = av_get_default_channel_layout(mCtx->channels);

    return ERR_OK;
}

void SoftFFmpegAudio::updateTimeStamp(OMX_BUFFERHEADERTYPE *inHeader) {
    CHECK_EQ(mInputBufferSize, 0);

    //XXX reset to AV_NOPTS_VALUE if the pts is invalid
    if (inHeader->nTimeStamp == SF_NOPTS_VALUE) {
        inHeader->nTimeStamp = AV_NOPTS_VALUE;
    }

    //update the audio clock if the pts is valid
    if (inHeader->nTimeStamp != AV_NOPTS_VALUE) {
        setAudioClock(inHeader->nTimeStamp);
    }
}

void SoftFFmpegAudio::initPacket(AVPacket *pkt,
        OMX_BUFFERHEADERTYPE *inHeader) {
    memset(pkt, 0, sizeof(AVPacket));
    av_init_packet(pkt);

    if (inHeader) {
        pkt->data = (uint8_t *)inHeader->pBuffer + inHeader->nOffset;
        pkt->size = inHeader->nFilledLen;
        pkt->pts = inHeader->nTimeStamp; //ingore it, we will compute it
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

int32_t SoftFFmpegAudio::decodeAudio() {
    int len = 0;
    int gotFrm = false;
    int32_t ret = ERR_OK;
    int32_t inputBufferUsedLength = 0;
    bool is_flush = (mEOSStatus == OUTPUT_FRAMES_FLUSHED);
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    BufferInfo *inInfo = NULL;
    OMX_BUFFERHEADERTYPE *inHeader = NULL;

    CHECK_EQ(mResampledDataSize, 0);

    if (!is_flush && !inQueue.empty()) {
        inInfo = *inQueue.begin();
        if (inInfo != NULL)  {
            inHeader = inInfo->mHeader;

            if (mInputBufferSize == 0) {
                updateTimeStamp(inHeader);
                mInputBufferSize = inHeader->nFilledLen;
            }
        }
    }

    if (mEOSStatus == INPUT_EOS_SEEN && (!inHeader || inHeader->nFilledLen == 0)
        && !(mCtx->codec->capabilities & CODEC_CAP_DELAY)) {
        return ERR_FLUSHED;
    }

    AVPacket pkt;
    initPacket(&pkt, inHeader);
    av_frame_unref(mFrame);

    len = avcodec_decode_audio4(mCtx, mFrame, &gotFrm, &pkt);
    av_packet_unref(&pkt);

    //a negative error code is returned if an error occurred during decoding
    if (len < 0) {
        ALOGW("ffmpeg audio decoder err, we skip the frame and play silence instead");
        mResampledData = mSilenceBuffer;
        mResampledDataSize = kOutputBufferSize;
        ret = ERR_OK;
    } else {
#if DEBUG_PKT
        ALOGV("ffmpeg audio decoder, consume pkt len: %d", len);
#endif
        if (!gotFrm) {
#if DEBUG_FRM
            ALOGI("ffmpeg audio decoder failed to get frame.");
#endif
            //stop sending empty packets if the decoder is finished
            if (is_flush && mCtx->codec->capabilities & CODEC_CAP_DELAY) {
                ALOGI("ffmpeg audio decoder failed to get more frames when flush.");
                ret = ERR_FLUSHED;
            } else {
                ret = ERR_NO_FRM;
            }
        } else {
            ret = resampleAudio();
        }
    }

    if (!is_flush) {
        if (len < 0) {
            //if error, we skip the frame 
            inputBufferUsedLength = mInputBufferSize;
        } else {
            inputBufferUsedLength = len;
        }
        mInputBufferSize -= inputBufferUsedLength;

        if (inHeader != NULL) {
            CHECK_GE(inHeader->nFilledLen, inputBufferUsedLength);
            inHeader->nOffset += inputBufferUsedLength;
            inHeader->nFilledLen -= inputBufferUsedLength;

            if (inHeader->nFilledLen == 0) {
                CHECK_EQ(mInputBufferSize, 0);
                inQueue.erase(inQueue.begin());
                inInfo->mOwnedByUs = false;
                notifyEmptyBufferDone(inHeader);
            }
        }
    }

    return ret;
}

int32_t SoftFFmpegAudio::resampleAudio() {
    int channels = 0;
    int64_t channelLayout = 0;
    size_t dataSize = 0;

    dataSize = av_samples_get_buffer_size(NULL, av_frame_get_channels(mFrame),
            mFrame->nb_samples, (enum AVSampleFormat)mFrame->format, 1);

#if DEBUG_FRM
    ALOGV("ffmpeg audio decoder, nb_samples:%d, get buffer size:%d",
            mFrame->nb_samples, dataSize);
#endif

    channels = av_get_channel_layout_nb_channels(mFrame->channel_layout);
    channelLayout =
        (mFrame->channel_layout && av_frame_get_channels(mFrame) == channels) ?
        mFrame->channel_layout : av_get_default_channel_layout(av_frame_get_channels(mFrame));

    // Create if we're reconfiguring, if the format changed mid-stream, or
    // if the output format is actually different
    if ((mReconfiguring && mSwrCtx) || (!mSwrCtx
            && (mFrame->format != mAudioSrcFmt
                || channelLayout != mAudioSrcChannelLayout
                || (unsigned int)mFrame->sample_rate != mAudioSrcFreq
                || mAudioSrcFmt != mAudioTgtFmt
                || mAudioSrcChannelLayout != mAudioTgtChannelLayout
                || mAudioSrcFreq != mAudioTgtFreq))) {
        if (mSwrCtx) {
            swr_free(&mSwrCtx);
        }
        mSwrCtx = swr_alloc_set_opts(NULL,
                mAudioTgtChannelLayout, mAudioTgtFmt,                     mAudioTgtFreq,
                channelLayout,       (enum AVSampleFormat)mFrame->format, mFrame->sample_rate,
                0, NULL);
        if (!mSwrCtx || swr_init(mSwrCtx) < 0) {
            ALOGE("Cannot create sample rate converter for conversion "
                    "of %d Hz %s %d channels to %d Hz %s %d channels!",
                    mFrame->sample_rate,
                    av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
                    av_frame_get_channels(mFrame),
                    mAudioTgtFreq,
                    av_get_sample_fmt_name(mAudioTgtFmt),
                    mAudioTgtChannels);
            return ERR_SWR_INIT_FAILED;
        }

        char src_layout_name[1024] = {0};
        char tgt_layout_name[1024] = {0};
        av_get_channel_layout_string(src_layout_name, sizeof(src_layout_name),
                mCtx->channels, channelLayout);
        av_get_channel_layout_string(tgt_layout_name, sizeof(tgt_layout_name),
                mAudioTgtChannels, mAudioTgtChannelLayout);
        ALOGI("Create sample rate converter for conversion "
                "of %d Hz %s %d channels(%s) "
                "to %d Hz %s %d channels(%s)!",
                mFrame->sample_rate,
                av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
                av_frame_get_channels(mFrame),
                src_layout_name,
                mAudioTgtFreq,
                av_get_sample_fmt_name(mAudioTgtFmt),
                mAudioTgtChannels,
                tgt_layout_name);

        mAudioSrcChannelLayout = channelLayout;
        mAudioSrcChannels = av_frame_get_channels(mFrame);
        mAudioSrcFreq = mFrame->sample_rate;
        mAudioSrcFmt = (enum AVSampleFormat)mFrame->format;
        mReconfiguring = false;
    }

    if (mSwrCtx) {
        const uint8_t **in = (const uint8_t **)mFrame->extended_data;
        uint8_t *out[] = {mAudioBuffer};
        int out_count = sizeof(mAudioBuffer) / mAudioTgtChannels / av_get_bytes_per_sample(mAudioTgtFmt);
        int out_size  = av_samples_get_buffer_size(NULL, mAudioTgtChannels, out_count, mAudioTgtFmt, 0);
        int len2 = 0;
        if (out_size < 0) {
            ALOGE("av_samples_get_buffer_size() failed");
            return ERR_INVALID_PARAM;
        }

        len2 = swr_convert(mSwrCtx, out, out_count, in, mFrame->nb_samples);
        if (len2 < 0) {
            ALOGE("audio_resample() failed");
            return ERR_RESAMPLE_FAILED;
        }
        if (len2 == out_count) {
            ALOGE("warning: audio buffer is probably too small");
            swr_init(mSwrCtx);
        }
        mResampledData = mAudioBuffer;
        mResampledDataSize = len2 * mAudioTgtChannels * av_get_bytes_per_sample(mAudioTgtFmt);

#if DEBUG_FRM
        ALOGV("ffmpeg audio decoder(resample), mFrame->nb_samples:%d, len2:%d, mResampledDataSize:%d, "
                "src channel:%u, src fmt:%s, tgt channel:%u, tgt fmt:%s",
                mFrame->nb_samples, len2, mResampledDataSize,
                av_frame_get_channels(mFrame),
                av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
                mAudioTgtChannels,
                av_get_sample_fmt_name(mAudioTgtFmt));
#endif
    } else {
        mResampledData = mFrame->data[0];
        mResampledDataSize = dataSize;

#if DEBUG_FRM
    ALOGV("ffmpeg audio decoder(no resample),"
            "nb_samples(before resample):%d, mResampledDataSize:%d",
            mFrame->nb_samples, mResampledDataSize);
#endif
    }

    return ERR_OK;
}

void SoftFFmpegAudio::drainOneOutputBuffer() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    BufferInfo *outInfo = *outQueue.begin();
    CHECK(outInfo != NULL);
    OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    BufferInfo *inInfo = *inQueue.begin();
    OMX_BUFFERHEADERTYPE *inHeader = NULL;

    if (inHeader != NULL) {
        inHeader = inInfo->mHeader;
    }

    CHECK_GT(mResampledDataSize, 0);

    size_t copy = mResampledDataSize;
    if (mResampledDataSize > kOutputBufferSize) {
        copy = kOutputBufferSize;
    }

    outHeader->nOffset = 0;
    outHeader->nFilledLen = copy;
    outHeader->nTimeStamp = getAudioClock();
    memcpy(outHeader->pBuffer, mResampledData, copy);
    outHeader->nFlags = 0;

    //update mResampledSize
    mResampledData += copy;
    mResampledDataSize -= copy;

    //update audio pts
    size_t frames = copy / (av_get_bytes_per_sample(mAudioTgtFmt) * mAudioTgtChannels);
    setAudioClock(getAudioClock() + ((frames * 1000000ll) / mAudioTgtFreq));

#if DEBUG_FRM
    ALOGV("ffmpeg audio decoder, fill out buffer, copy:%u, pts: %lld, clock: %lld",
            copy, outHeader->nTimeStamp, getAudioClock());
#endif

    outQueue.erase(outQueue.begin());
    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);
}

void SoftFFmpegAudio::drainEOSOutputBuffer() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    BufferInfo *outInfo = *outQueue.begin();
    CHECK(outInfo != NULL);
    OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

    // CHECK_EQ(mResampledDataSize, 0);

    ALOGD("ffmpeg audio decoder fill eos outbuf");

    outHeader->nTimeStamp = getAudioClock();
    outHeader->nFilledLen = 0;
    outHeader->nFlags = OMX_BUFFERFLAG_EOS;

    outQueue.erase(outQueue.begin());
    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);

    mEOSStatus = OUTPUT_FRAMES_FLUSHED;
}

void SoftFFmpegAudio::drainAllOutputBuffers() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    if (!mCodecAlreadyOpened) {
        drainEOSOutputBuffer();
        mEOSStatus = OUTPUT_FRAMES_FLUSHED;
        return;
    }

    while (!outQueue.empty()) {
        if (mResampledDataSize == 0) {
            int32_t err = decodeAudio();
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
        }

        if (mResampledDataSize > 0) {
            drainOneOutputBuffer();
        }
    }
}

void SoftFFmpegAudio::onQueueFilled(OMX_U32 /* portIndex */) {
    BufferInfo *inInfo = NULL;
    OMX_BUFFERHEADERTYPE *inHeader = NULL;

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

        inInfo   = *inQueue.begin();
        inHeader = inInfo->mHeader;

        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            ALOGD("ffmpeg audio decoder eos");
            mEOSStatus = INPUT_EOS_SEEN;
            continue;
        }

        if (inHeader->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
            if (handleExtradata() != ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }
            continue;
        }

        if (!mCodecAlreadyOpened) {
            if (openDecoder() != ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }
        }

        if (mResampledDataSize == 0) {
            int32_t err = decodeAudio();
            if (err < ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            } else if (err == ERR_NO_FRM) {
                CHECK_EQ(mResampledDataSize, 0);
                continue;
            } else {
                CHECK_EQ(err, ERR_OK);
            }
        }

        if (mResampledDataSize > 0) {
            drainOneOutputBuffer();
        }
    }
}

void SoftFFmpegAudio::onPortFlushCompleted(OMX_U32 portIndex) {
    ALOGV("ffmpeg audio decoder flush port(%u)", portIndex);
    if (portIndex == kInputPortIndex) {
        if (mCtx && avcodec_is_open(mCtx)) {
            //Make sure that the next buffer output does not still
            //depend on fragments from the last one decoded.
            avcodec_flush_buffers(mCtx);
        }

        setAudioClock(0);
        mInputBufferSize = 0;
        mResampledDataSize = 0;
        mResampledData = NULL;
        mEOSStatus = INPUT_DATA_AVAILABLE;
    }
}

void SoftFFmpegAudio::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
    if (portIndex != kOutputPortIndex) {
        return;
    }

    switch (mOutputPortSettingsChange) {
        case NONE:
            break;

        case AWAITING_DISABLED:
        {
            CHECK(!enabled);
            mOutputPortSettingsChange = AWAITING_ENABLED;
            break;
        }

        default:
        {
            CHECK_EQ((int)mOutputPortSettingsChange, (int)AWAITING_ENABLED);
            CHECK(enabled);
            mOutputPortSettingsChange = NONE;
            break;
        }
    }
}

int64_t SoftFFmpegAudio::getAudioClock() {
    if (sAudioClock == NULL) {
        sAudioClock = (int64_t*) malloc(sizeof(int64_t));
        *sAudioClock = 0;
    }
    ALOGV("getAudioClock: %lld", *sAudioClock);
    return *sAudioClock;
}

void SoftFFmpegAudio::setAudioClock(int64_t ticks) {
    if (sAudioClock == NULL) {
        sAudioClock = (int64_t*) malloc(sizeof(int64_t));
    }
    *sAudioClock = ticks;
}

void SoftFFmpegAudio::onReset() {
    enum AVCodecID codecID = mCtx->codec_id;
    deInitDecoder();
    initDecoder(codecID);
    mSignalledError = false;
    mOutputPortSettingsChange = NONE;
    mEOSStatus = INPUT_DATA_AVAILABLE;
}

SoftOMXComponent* SoftFFmpegAudio::createSoftOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    OMX_AUDIO_CODINGTYPE codingType = OMX_AUDIO_CodingAutoDetect;
    char *componentRole = NULL;
    enum AVCodecID codecID = AV_CODEC_ID_NONE;

    for (size_t i = 0; i < kNumAudioComponents; ++i) {
        if (!strcasecmp(name, kAudioComponents[i].mName)) {
            componentRole = strdup(kAudioComponents[i].mRole);
            codingType = kAudioComponents[i].mAudioCodingType;
            codecID = kAudioComponents[i].mCodecID;
            break;
         }
     }

    return new SoftFFmpegAudio(name, componentRole, codingType, codecID,
            callbacks, appData, component);
}

}  // namespace android
