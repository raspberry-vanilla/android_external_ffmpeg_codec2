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

#define LOG_TAG "C2FFMPEGAudioDecodeComponent"
#include <android-base/stringprintf.h>
#include <log/log.h>

#include <SimpleC2Interface.h>
#include "C2FFMPEGAudioDecodeComponent.h"
#include <libswresample/swresample_internal.h>

#define DEBUG_FRAMES 0
#define DEBUG_EXTRADATA 0

namespace android {

static enum AVSampleFormat convertFormatToFFMPEG(C2Config::pcm_encoding_t encoding) {
    switch (encoding) {
        case C2Config::PCM_8: return AV_SAMPLE_FMT_U8;
        case C2Config::PCM_16: return AV_SAMPLE_FMT_S16;
        case C2Config::PCM_32: return AV_SAMPLE_FMT_S32;
        case C2Config::PCM_FLOAT: return AV_SAMPLE_FMT_FLT;
        default: return AV_SAMPLE_FMT_NONE;
    }
}

__unused
static C2Config::pcm_encoding_t convertFormatToC2(enum AVSampleFormat format) {
    switch (format) {
        case AV_SAMPLE_FMT_U8: return C2Config::PCM_8;
        case AV_SAMPLE_FMT_S16: return C2Config::PCM_16;
        case AV_SAMPLE_FMT_S32: return C2Config::PCM_32;
        case AV_SAMPLE_FMT_FLT: return C2Config::PCM_FLOAT;
        default: return C2Config::PCM_16;
    }
}

// Helper structures to encapsulate the specific codec behaviors.
// Currently only used to process extradata.

struct CodecHelper {
    virtual ~CodecHelper() {}
    virtual c2_status_t onCodecConfig(AVCodecContext* mCtx, C2ReadView* inBuffer);
    virtual c2_status_t onOpen(AVCodecContext* mCtx);
    virtual c2_status_t onOpened(AVCodecContext* mCtx);
};

c2_status_t CodecHelper::onCodecConfig(AVCodecContext* mCtx, C2ReadView* inBuffer) {
    int orig_extradata_size = mCtx->extradata_size;
    int add_extradata_size = inBuffer->capacity();

#if DEBUG_EXTRADATA
    ALOGD("CodecHelper::onCodecConfig: add = %u, current = %d", add_extradata_size, orig_extradata_size);
#endif
    mCtx->extradata_size += add_extradata_size;
    mCtx->extradata = (uint8_t *) realloc(mCtx->extradata, mCtx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (! mCtx->extradata) {
        ALOGE("CodecHelper::onCodecConfig: ffmpeg audio decoder failed to alloc extradata memory.");
        return C2_NO_MEMORY;
    }
    memcpy(mCtx->extradata + orig_extradata_size, inBuffer->data(), add_extradata_size);
    memset(mCtx->extradata + mCtx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    return C2_OK;
}

c2_status_t CodecHelper::onOpen(AVCodecContext* mCtx) {
#if DEBUG_EXTRADATA
    ALOGD("CodecHelper::onOpen: extradata = %d", mCtx->extradata_size);
#else
    // Silence compilation warning.
    (void)mCtx;
#endif
    return C2_OK;
}

c2_status_t CodecHelper::onOpened(AVCodecContext* mCtx) {
    (void)mCtx;
    return C2_OK;
}

struct VorbisCodecHelper : public CodecHelper {
    VorbisCodecHelper();
    ~VorbisCodecHelper();
    c2_status_t onCodecConfig(AVCodecContext* mCtx, C2ReadView* rView);
    c2_status_t onOpen(AVCodecContext* mCtx);

    uint8_t* mHeader[3];
    int mHeaderLen[3];
};

VorbisCodecHelper::VorbisCodecHelper()
     : CodecHelper(),
       mHeader{ NULL, NULL, NULL },
       mHeaderLen{ 0, 0, 0 } {
}

VorbisCodecHelper::~VorbisCodecHelper() {
    for (int i = 0; i < 3; i++) {
        if (mHeader[i]) {
            av_free(mHeader[i]);
            mHeader[i] = NULL;
        }
        mHeaderLen[i] = 0;
    }
}

c2_status_t VorbisCodecHelper::onCodecConfig(AVCodecContext* mCtx __unused, C2ReadView* inBuffer) {
    const uint8_t* data = inBuffer->data();
    int len = inBuffer->capacity();
    int index = 0;

    switch (data[0]) {
        case 1: index = 0; break;
        case 3: index = 1; break;
        case 5: index = 2; break;
        default:
            ALOGE("VorbisCodecHelper::onCodecConfig: invalid vorbis codec config (%02x)", data[0]);
            return C2_BAD_VALUE;
    }

    if (mHeader[index]) {
        ALOGW("VorbisCodecHelper::onCodecConfig: overwriting header[%d]", index);
        av_free(mHeader[index]);
    }
    mHeader[index] = (uint8_t*)av_mallocz(len);
    if (! mHeader[index]) {
        ALOGE("VorbisCodecHelper::onCodecConfig: oom for vorbis extradata");
        return C2_NO_MEMORY;
    }
    memcpy(mHeader[index], data, len);
    mHeaderLen[index] = len;

#if DEBUG_EXTRADATA
    ALOGD("VorbisCodecHelper::onCodecConfig: found header[%d] = %d", index, len);
#endif

    return C2_OK;
}

c2_status_t VorbisCodecHelper::onOpen(AVCodecContext* mCtx) {
    // Don't generate extradata twice
    if (! mCtx->extradata) {
        if (! setup_vorbis_extradata(&mCtx->extradata,
                                     &mCtx->extradata_size,
                                     (const uint8_t**)mHeader,
                                     mHeaderLen)) {
            return C2_NO_MEMORY;
        }
#if DEBUG_EXTRADATA
        ALOGD("VorbisCodecHelper::onOpen: extradata = %d", mCtx->extradata_size);
#endif
    }
    return C2_OK;
}

struct Ac3CodecHelper : public CodecHelper {
    c2_status_t onOpened(AVCodecContext* mCtx);
};

c2_status_t Ac3CodecHelper::onOpened(AVCodecContext* mCtx) {
    int err = av_opt_set_chlayout(mCtx->priv_data, "downmix", &mCtx->ch_layout, 0);
    if (err < 0) {
        ALOGE("Ac3CodecHelper::onOpened: failed to set downmix = %d: %s (%08x)",
              mCtx->ch_layout.nb_channels, av_err2str(err), err);
    } else {
        ALOGD("Ac3CodecHelper::onOpened: set downmix = %d", mCtx->ch_layout.nb_channels);
    }
    return C2_OK;
}

CodecHelper* createCodecHelper(enum AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_AC3:
        case AV_CODEC_ID_EAC3:
            return new Ac3CodecHelper();
        case AV_CODEC_ID_VORBIS:
            return new VorbisCodecHelper();
        default:
            return new CodecHelper();
    }
}

C2FFMPEGAudioDecodeComponent::C2FFMPEGAudioDecodeComponent(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2FFMPEGAudioDecodeInterface>& intf)
    : SimpleC2Component(std::make_shared<SimpleInterface<C2FFMPEGAudioDecodeInterface>>(componentInfo->name, 0, intf)),
      mInfo(componentInfo),
      mIntf(intf),
      mCodecID(componentInfo->codecID),
      mCtx(NULL),
      mFrame(NULL),
      mPacket(NULL),
      mFFMPEGInitialized(false),
      mCodecAlreadyOpened(false),
      mEOSSignalled(false),
      mSwrCtx(NULL),
      mTargetSampleFormat(AV_SAMPLE_FMT_NONE),
      mTargetSampleRate(44100),
      mTargetChannels(1) {
    ALOGD("C2FFMPEGAudioDecodeComponent: mediaType = %s", componentInfo->mediaType);
}

C2FFMPEGAudioDecodeComponent::~C2FFMPEGAudioDecodeComponent() {
    ALOGD("~C2FFMPEGAudioDecodeComponent: mCtx = %p", mCtx);
    onRelease();
}

c2_status_t C2FFMPEGAudioDecodeComponent::initDecoder() {
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

    mCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    mCtx->codec_id = mCodecID;

    updateAudioParameters();

    av_channel_layout_default(&mCtx->ch_layout, mTargetChannels);
    mCtx->sample_rate = mTargetSampleRate;
    mCtx->bit_rate = 0;
    mCtx->sample_fmt = mTargetSampleFormat;

    // Avoid resampling if possible, ask the codec for the target format.
    mCtx->request_sample_fmt = mCtx->sample_fmt;

    const FFMPEGAudioCodecInfo* codecInfo = mIntf->getCodecInfo();

    if (codecInfo) {
        ALOGD("initDecoder: use codec info from extractor");
        mCtx->codec_id = (enum AVCodecID)codecInfo->codec_id;
        mCtx->bit_rate = mIntf->getBitrate(); // The extractor always sets bitrate
        mCtx->bits_per_coded_sample = codecInfo->bits_per_coded_sample;
        mCtx->block_align = codecInfo->block_align;
        // FIXME: Is more needed...?
    }

    mCodecHelper = createCodecHelper(mCtx->codec_id);

    ALOGD("initDecoder: %p [%s], %s - sr=%d, ch=%d, fmt=%s",
          mCtx, avcodec_get_name(mCtx->codec_id), mInfo->mediaType,
          mCtx->sample_rate, mCtx->ch_layout.nb_channels, av_get_sample_fmt_name(mCtx->sample_fmt));

    return C2_OK;
}

c2_status_t C2FFMPEGAudioDecodeComponent::openDecoder() {
    if (mCodecAlreadyOpened) {
        return C2_OK;
    }

    mCodecHelper->onOpen(mCtx);

    // Find decoder
    mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
    if (! mCtx->codec) {
        ALOGE("openDecoder: ffmpeg audio decoder failed to find codec %d", mCtx->codec_id);
        return C2_NOT_FOUND;
    }

    // Configure decoder
    mCtx->workaround_bugs   = 1;
    mCtx->idct_algo         = 0;
    mCtx->skip_frame        = AVDISCARD_DEFAULT;
    mCtx->skip_idct         = AVDISCARD_DEFAULT;
    mCtx->skip_loop_filter  = AVDISCARD_DEFAULT;
    mCtx->error_concealment = 3;

    mCtx->flags |= AV_CODEC_FLAG_BITEXACT;

    ALOGD("openDecoder: begin to open ffmpeg audio decoder(%s), mCtx sample_rate: %d, channels: %d",
           avcodec_get_name(mCtx->codec_id), mCtx->sample_rate, mCtx->ch_layout.nb_channels);

    int err = avcodec_open2(mCtx, mCtx->codec, NULL);
    if (err < 0) {
        ALOGE("openDecoder: ffmpeg audio decoder failed to initialize.(%s)", av_err2str(err));
        return C2_NO_INIT;
    }
    mCodecAlreadyOpened = true;

    mCodecHelper->onOpened(mCtx);

    ALOGD("openDecoder: open ffmpeg audio decoder(%s) success, mCtx sample_rate: %d, "
          "channels: %d, sample_fmt: %s, bits_per_coded_sample: %d, bits_per_raw_sample: %d",
          avcodec_get_name(mCtx->codec_id),
          mCtx->sample_rate, mCtx->ch_layout.nb_channels,
          av_get_sample_fmt_name(mCtx->sample_fmt),
          mCtx->bits_per_coded_sample, mCtx->bits_per_raw_sample);

    mFrame = av_frame_alloc();
    if (! mFrame) {
        ALOGE("openDecoder: oom for audio frame");
        return C2_NO_MEMORY;
    }

    return C2_OK;
}

void C2FFMPEGAudioDecodeComponent::deInitDecoder() {
    ALOGD("deInitDecoder: %p", mCtx);
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
            mCodecAlreadyOpened = false;
        }
        av_freep(&mCtx);
    }
    if (mFrame) {
        av_frame_free(&mFrame);
        mFrame = NULL;
    }
    if (mPacket) {
        av_packet_free(&mPacket);
        mPacket = NULL;
    }
    if (mSwrCtx) {
        swr_free(&mSwrCtx);
    }
    if (mCodecHelper) {
        delete mCodecHelper;
        mCodecHelper = NULL;
    }
    mEOSSignalled = false;
}

c2_status_t C2FFMPEGAudioDecodeComponent::processCodecConfig(C2ReadView* inBuffer) {
#if DEBUG_EXTRADATA
    ALOGD("processCodecConfig: inBuffer = %d", inBuffer->capacity());
#endif
    if (! mCodecAlreadyOpened) {
        return mCodecHelper->onCodecConfig(mCtx, inBuffer);
    } else {
        ALOGW("processCodecConfig: decoder is already opened, ignoring %d bytes", inBuffer->capacity());
    }

    return C2_OK;
}

c2_status_t C2FFMPEGAudioDecodeComponent::sendInputBuffer(
        C2ReadView *inBuffer, int64_t timestamp) {
    if (!mPacket) {
        mPacket = av_packet_alloc();
        if (!mPacket) {
            ALOGE("sendInputBuffer: oom for audio packet");
            return C2_NO_MEMORY;
        }
    }

    mPacket->data = inBuffer ? const_cast<uint8_t *>(inBuffer->data()) : NULL;
    mPacket->size = inBuffer ? inBuffer->capacity() : 0;
    mPacket->pts = timestamp;
    mPacket->dts = timestamp;

    int err = avcodec_send_packet(mCtx, mPacket);
    av_packet_unref(mPacket);

    if (err < 0) {
        ALOGE("sendInputBuffer: failed to send data to decoder err = %d", err);
        // Don't report error to client.
    }

    return C2_OK;
}

c2_status_t C2FFMPEGAudioDecodeComponent::receiveFrame(bool* hasFrame) {
    int err = avcodec_receive_frame(mCtx, mFrame);

    if (err == 0) {
        *hasFrame = true;
    } else if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
        *hasFrame = false;
    } else {
        ALOGE("receiveFrame: failed to receive frame from decoder err = %d", err);
        // Don't report error to client.
    }

    return C2_OK;
}

c2_status_t C2FFMPEGAudioDecodeComponent::getOutputBuffer(C2WriteView* outBuffer) {
    if (! mSwrCtx ||
        mSwrCtx->in_sample_fmt != mFrame->format ||
        mSwrCtx->in_sample_rate != mFrame->sample_rate ||
        av_channel_layout_compare(&mSwrCtx->in_ch_layout, &mFrame->ch_layout) != 0 ||
        mSwrCtx->out_sample_fmt != mTargetSampleFormat ||
        mSwrCtx->out_sample_rate != mTargetSampleRate ||
        mSwrCtx->out_ch_layout.nb_channels != mTargetChannels) {
        if (mSwrCtx) {
            swr_free(&mSwrCtx);
        }

        AVChannelLayout newLayout;

        av_channel_layout_default(&newLayout, mTargetChannels);
        swr_alloc_set_opts2(&mSwrCtx,
                            &newLayout, mTargetSampleFormat, mTargetSampleRate,
                            &mFrame->ch_layout, (enum AVSampleFormat)mFrame->format, mFrame->sample_rate,
                            0, NULL);
        av_channel_layout_uninit(&newLayout);
        if (! mSwrCtx || swr_init(mSwrCtx) < 0) {
            ALOGE("getOutputBuffer: cannot create audio converter - sr=%d, ch=%d, fmt=%s => sr=%d, ch=%d, fmt=%s",
                  mFrame->sample_rate, mFrame->ch_layout.nb_channels, av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
                  mTargetSampleRate, mTargetChannels, av_get_sample_fmt_name(mTargetSampleFormat));
            if (mSwrCtx) {
                swr_free(&mSwrCtx);
            }
            return C2_NO_MEMORY;
        }

        ALOGD("getOutputBuffer: created audio converter - sr=%d, ch=%d, fmt=%s => sr=%d, ch=%d, fmt=%s",
              mFrame->sample_rate, mFrame->ch_layout.nb_channels, av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
              mTargetSampleRate, mTargetChannels, av_get_sample_fmt_name(mTargetSampleFormat));
    }

    uint8_t* out[1] = { outBuffer->data() };
    int ret = swr_convert(mSwrCtx, out, mFrame->nb_samples, (const uint8_t**)mFrame->extended_data, mFrame->nb_samples);

    if (ret < 0) {
        ALOGE("getOutputBuffer: audio conversion failed");
        return C2_CORRUPTED;
    } else if (ret != mFrame->nb_samples) {
        ALOGW("getOutputBuffer: audio conversion truncated!");
    }

#if DEBUG_FRAMES
    ALOGD("getOutputBuffer: audio converted - sr=%d, ch=%d, fmt=%s, #=%d => sr=%d, ch=%d, fmt=%s, #=%d(%d)",
          mFrame->sample_rate, mFrame->ch_layout.nb_channels, av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format), mFrame->nb_samples,
          mTargetSampleRate, mTargetChannels, av_get_sample_fmt_name(mTargetSampleFormat), mFrame->nb_samples, outBuffer->capacity());
#endif

    return C2_OK;
} 

void C2FFMPEGAudioDecodeComponent::updateAudioParameters() {
    mTargetSampleFormat = convertFormatToFFMPEG(mIntf->getPcmEncodingInfo());
    mTargetSampleRate = mIntf->getSampleRate();
    mTargetChannels = mIntf->getChannelCount();
}

c2_status_t C2FFMPEGAudioDecodeComponent::onInit() {
    ALOGD("onInit");
    return initDecoder();
}

c2_status_t C2FFMPEGAudioDecodeComponent::onStop() {
    ALOGD("onStop");
    return C2_OK;
}

void C2FFMPEGAudioDecodeComponent::onReset() {
    ALOGD("onReset");
    deInitDecoder();
    initDecoder();
}

void C2FFMPEGAudioDecodeComponent::onRelease() {
    ALOGD("onRelease");
    deInitDecoder();
    if (mFFMPEGInitialized) {
        deInitFFmpeg();
        mFFMPEGInitialized = false;
    }
}

c2_status_t C2FFMPEGAudioDecodeComponent::onFlush_sm() {
    ALOGD("onFlush_sm");
    if (mCtx && avcodec_is_open(mCtx)) {
        // Make sure that the next buffer output does not still
        // depend on fragments from the last one decoded.
        avcodec_flush_buffers(mCtx);
        mEOSSignalled = false;
    }
    return C2_OK;
}

void C2FFMPEGAudioDecodeComponent::process(
    const std::unique_ptr<C2Work> &work,
    const std::shared_ptr<C2BlockPool>& pool
) {
    size_t inSize = 0u;
    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM);
    C2ReadView rView = mDummyReadView;
    bool hasInputBuffer = false;
    bool hasFrame = false;

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
    // NOTE: This has an impact on the drain operation.

    work->result = C2_OK;
    work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;

    if (inSize || (eos && mCodecAlreadyOpened)) {
        c2_status_t err = C2_OK;

        if (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
            work->result = processCodecConfig(&rView);
            return;
        }

        if (! mCodecAlreadyOpened) {
            err = openDecoder();
            if (err != C2_OK) {
                work->result = err;
                return;
            }
        }

        err = sendInputBuffer(&rView, work->input.ordinal.timestamp.peekll());
        if (err != C2_OK) {
            work->result = err;
            return;
        }

        while (true) {
            hasFrame = false;
            err = receiveFrame(&hasFrame);
            if (err != C2_OK) {
                work->result = err;
                return;
            }

            if (! hasFrame) {
                break;
            }

#if DEBUG_FRAMES
            ALOGD("process: got frame pts=%" PRId64 " dts=%" PRId64 " ts=%" PRId64 " - sr=%d, ch=%d, fmt=%s, #=%d",
                  mFrame->pts, mFrame->pkt_dts, mFrame->best_effort_timestamp,
                  mFrame->sample_rate, mFrame->channels, av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
                  mFrame->nb_samples);
#endif
            // Always target the sample format on output port. Even if we can trigger a config update
            // for the sample format, Android does not support planar formats, so if the codec uses
            // such format (e.g. AC3), conversion is needed. Technically we can limit the conversion to
            // planer->packed, but that means Android would also do its own conversion to the wanted
            // format on output port. To avoid double conversion, target directly the wanted format.

            bool needConfigUpdate = (mFrame->sample_rate != mTargetSampleRate ||
                                     mFrame->ch_layout.nb_channels != mTargetChannels);
            bool needResampling = (needConfigUpdate ||
                                   mFrame->format != mTargetSampleFormat ||
                                   // We only support sending audio data to Android in native order.
                                   mFrame->ch_layout.order != AV_CHANNEL_ORDER_NATIVE);

            if (needConfigUpdate) {
                ALOGD("process: audio params changed - sr=%d, ch=%d, fmt=%s => sr=%d, ch=%d, fmt=%s",
                      mTargetSampleRate, mTargetChannels, av_get_sample_fmt_name(mTargetSampleFormat),
                      mFrame->sample_rate, mFrame->ch_layout.nb_channels, av_get_sample_fmt_name(mTargetSampleFormat));

                if (work->worklets.front()->output.buffers.size() > 0) {
                    // Not sure if this would ever happen, nor how to handle it...
                    ALOGW("process: audio params changed with non empty output buffers pending");
                }

                C2StreamSampleRateInfo::output sampleRate(0u, mFrame->sample_rate);
                C2StreamChannelCountInfo::output channelCount(0u, mFrame->ch_layout.nb_channels);
                std::vector<std::unique_ptr<C2SettingResult>> failures;

                err = mIntf->config({ &sampleRate, &channelCount }, C2_MAY_BLOCK, &failures);
                if (err == C2_OK) {
                    work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(sampleRate));
                    work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(channelCount));
                    updateAudioParameters();
                } else {
                    ALOGE("process: config update failed err = %d", err);
                    work->result = C2_CORRUPTED;
                    return;
                }
            }

            std::shared_ptr<C2LinearBlock> block;
            int len = av_samples_get_buffer_size(NULL, mTargetChannels, mFrame->nb_samples, mTargetSampleFormat, 0);

            err = pool->fetchLinearBlock(len, { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE }, &block);
            if (err != C2_OK) {
                ALOGE("process: failed to fetch linear block for #=%d err = %d",
                      mFrame->nb_samples, err);
                work->result = C2_CORRUPTED;
                return;
            }

            C2WriteView wView = block->map().get();

            err = wView.error();
            if (err != C2_OK) {
                ALOGE("process: write view map failed err = %d", err);
                work->result = C2_CORRUPTED;
                return;
            }

            if (needResampling) {
                err = getOutputBuffer(&wView);
                if (err != C2_OK) {
                    work->result = err;
                    return;
                }
            }
            else {
#if DEBUG_FRAMES
                ALOGD("process: no audio conversion needed");
#endif
                memcpy(wView.data(), mFrame->data[0], mFrame->linesize[0]);
            }

            std::shared_ptr<C2Buffer> buffer = createLinearBuffer(std::move(block), 0, len);

            if (mCtx->codec->capabilities & AV_CODEC_CAP_SUBFRAMES) {
                auto fillWork = [buffer, &work, this](const std::unique_ptr<C2Work>& clone) {
                    clone->worklets.front()->output.configUpdate = std::move(work->worklets.front()->output.configUpdate);
                    clone->worklets.front()->output.buffers.clear();
                    clone->worklets.front()->output.buffers.push_back(buffer);
                    clone->worklets.front()->output.ordinal = clone->input.ordinal;
                    if (mFrame->best_effort_timestamp == AV_NOPTS_VALUE) {
                        work->worklets.front()->output.ordinal.timestamp = mFrame->best_effort_timestamp;
                    }
                    clone->worklets.front()->output.flags = C2FrameData::FLAG_INCOMPLETE;
                    clone->workletsProcessed = 1u;
                    clone->result = C2_OK;
                };

#if DEBUG_FRAMES
                ALOGD("process: send subframe buffer ts=%" PRIu64 " idx=%" PRIu64,
                      work->input.ordinal.timestamp.peeku(), work->input.ordinal.frameIndex.peeku());
#endif
                cloneAndSend(work->input.ordinal.frameIndex.peeku(), work, fillWork);
            }
            else {
                work->worklets.front()->output.buffers.push_back(buffer);
                if (mFrame->best_effort_timestamp == AV_NOPTS_VALUE) {
                    work->worklets.front()->output.ordinal.timestamp = mFrame->best_effort_timestamp;
                }
                break;
            }
        }
    }
#if DEBUG_FRAMES
    else {
        ALOGW("process: ignoring empty work");
    }
#endif

    if (eos) {
        mEOSSignalled = true;
        work->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
    }
}

c2_status_t C2FFMPEGAudioDecodeComponent::drain(
    uint32_t drainMode,
    const std::shared_ptr<C2BlockPool>& /* pool */
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

    bool hasFrame = false;
    c2_status_t err = C2_OK;

    while (err == C2_OK) {
        hasFrame = false;
        err = sendInputBuffer(NULL, 0);
        if (err == C2_OK) {
            err = receiveFrame(&hasFrame);
            if (hasFrame) {
                ALOGW("drain: skip frame pts=%" PRId64 " dts=%" PRId64 " ts=%" PRId64 " - sr=%d, ch=%d, fmt=%s, #=%d",
                      mFrame->pts, mFrame->pkt_dts, mFrame->best_effort_timestamp,
                      mFrame->sample_rate, mFrame->ch_layout.nb_channels, av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
                      mFrame->nb_samples);
            } else {
                err = C2_NOT_FOUND;
            }
        }
    }

    return C2_OK;
}

} // namespace android
