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

#ifndef C2_FFMPEG_AUDIO_DECODE_COMPONENT_H
#define C2_FFMPEG_AUDIO_DECODE_COMPONENT_H

#include <SimpleC2Component.h>
#include "C2FFMPEGCommon.h"
#include "C2FFMPEGAudioDecodeInterface.h"

namespace android {

struct CodecHelper;

class C2FFMPEGAudioDecodeComponent : public SimpleC2Component {
public:
    explicit C2FFMPEGAudioDecodeComponent(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2FFMPEGAudioDecodeInterface>& intf);
    virtual ~C2FFMPEGAudioDecodeComponent();

protected:
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) override;

private:
    c2_status_t initDecoder();
    c2_status_t openDecoder();
    void deInitDecoder();
    c2_status_t processCodecConfig(C2ReadView* inBuffer);
    c2_status_t sendInputBuffer(C2ReadView* inBuffer, int64_t timestamp);
    c2_status_t receiveFrame(bool* hasFrame);
    c2_status_t getOutputBuffer(C2WriteView* outBuffer);
    void updateAudioParameters();

private:
    const C2FFMPEGComponentInfo* mInfo;
    std::shared_ptr<C2FFMPEGAudioDecodeInterface> mIntf;
    enum AVCodecID mCodecID;
    AVCodecContext* mCtx;
    AVFrame* mFrame;
    AVPacket* mPacket;
    bool mFFMPEGInitialized;
    bool mCodecAlreadyOpened;
    bool mEOSSignalled;
    // Audio resampling
    struct SwrContext* mSwrCtx;
    enum AVSampleFormat mTargetSampleFormat;
    int mTargetSampleRate;
    int mTargetChannels;
    // Misc
    CodecHelper* mCodecHelper;
};

} // namespace android

#endif // C2_FFMPEG_AUDIO_DECODE_COMPONENT_H
