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

#ifndef C2_FFMPEG_VIDEO_DECODE_COMPONENT_H
#define C2_FFMPEG_VIDEO_DECODE_COMPONENT_H

#include <deque>
#include <utility>
#include <SimpleC2Component.h>
#include "C2FFMPEGCommon.h"
#include "C2FFMPEGVideoDecodeInterface.h"

namespace android {

typedef std::pair<uint64_t, uint64_t> PendingWork;

class C2FFMPEGVideoDecodeComponent : public SimpleC2Component {
public:
    explicit C2FFMPEGVideoDecodeComponent(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2FFMPEGVideoDecodeInterface>& intf);
    virtual ~C2FFMPEGVideoDecodeComponent();

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
    c2_status_t receiveFrame(bool* hasPicture);
    c2_status_t getOutputBuffer(C2GraphicView* outBuffer);
    c2_status_t outputFrame(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool);

    void pushPendingWork(const std::unique_ptr<C2Work>& work);
    void popPendingWork(const std::unique_ptr<C2Work>& work);
    void prunePendingWorksUntil(const std::unique_ptr<C2Work>& work);

private:
    const C2FFMPEGComponentInfo* mInfo;
    std::shared_ptr<C2FFMPEGVideoDecodeInterface> mIntf;
    enum AVCodecID mCodecID;
    AVCodecContext* mCtx;
    struct SwsContext *mImgConvertCtx;
    AVFrame* mFrame;
    AVPacket* mPacket;
    bool mFFMPEGInitialized;
    bool mCodecAlreadyOpened;
    bool mExtradataReady;
    bool mEOSSignalled;
    std::deque<PendingWork> mPendingWorkQueue;
};

} // namespace android

#endif // C2_FFMPEG_VIDEO_DECODE_COMPONENT_H
