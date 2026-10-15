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

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
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
    c2_status_t getOutputBuffer(C2GraphicView* outBuffer, bool p010);
    void initOutputPolicy();
    C2ColorAspectsStruct currentColorAspects() const;
    bool allow10BitOutput() const;
    void updateStreamInfo(std::vector<std::unique_ptr<C2Param>>* configUpdate);
    c2_status_t outputFrame(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool);

    void pushPendingWork(const std::unique_ptr<C2Work>& work);
    void popPendingWork(const std::unique_ptr<C2Work>& work);
    void prunePendingWorksUntil(const std::unique_ptr<C2Work>& work);

    // One frame deep output pipeline: the (memory bandwidth bound) conversion of
    // frame N runs on a worker thread while the next packet is submitted to the
    // decoder; frame N is delivered once the next frame arrives, on drain/EOS or flush.
    void submitJob();
    c2_status_t completeJob();
    void dropJob();
    void workerLoop();

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
    // Pixel format last reported to the framework (persists across reset,
    // like the interface parameter it mirrors).
    uint32_t mHalPixelFormat;
    // Pixel format requested by the client (Surface => IMPLEMENTATION_DEFINED,
    // ByteBuffer => YCBCR_420_888, or explicit P010), as opposed to a value
    // this component configured itself.
    uint32_t mClientPixelFormat;
    // persist.vendor.ffmpeg_codec2.p010: 0 = off, 1 = auto (HDR only), 2 = always
    int mP010Mode;
    // gralloc can allocate CPU-writable P010 buffers (and they did not fail).
    bool mP010Supported;
    // Decision taken in receiveFrame() for the current frame.
    bool mOutput10Bit;
    bool mFrameIsHw;      // mFrame is still an undecoded-to-CPU hardware frame
    bool mDirectNv12;     // convert hardware frames straight into the output buffer
    bool mColorAspectsSent;
    bool mHdrSent;
    C2StreamColorAspectsInfo::output mLastAspects;
    C2StreamHdrStaticInfo::output mLastHdr;
    std::deque<PendingWork> mPendingWorkQueue;

    struct OutputJob {
        AVFrame* hw = nullptr;                      // reference to the decoder frame
        std::shared_ptr<C2GraphicBlock> block;
        std::unique_ptr<C2GraphicView> view;        // keeps the block mapped
        uint32_t halFormat = 0;
        std::vector<std::unique_ptr<C2Param>> configUpdate;
        uint64_t frameIndex = 0;
        int width = 0;
        int height = 0;
        uint8_t* dstY = nullptr;
        int strideY = 0;
        uint8_t* dstC = nullptr;
        int strideC = 0;
        int result = 0;
    };
    bool mAsyncOutput;
    OutputJob mJob;
    bool mJobValid;     // a job was submitted and not yet completed
    bool mJobRun;       // worker has work to do
    bool mJobDone;      // worker finished the job
    bool mWorkerExit;
    std::mutex mJobMutex;
    std::condition_variable mJobCv;
    std::thread mWorker;
};

} // namespace android

#endif // C2_FFMPEG_VIDEO_DECODE_COMPONENT_H
