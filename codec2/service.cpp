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

//#define LOG_NDEBUG 0
#define LOG_TAG "android.hardware.media.c2@1.2-service"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <binder/ProcessState.h>
#include <codec2/hidl/1.2/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>
#include <minijail.h>

#include <util/C2InterfaceHelper.h>
#include <C2Component.h>
#include <C2Config.h>

#include "C2FFMPEGCommon.h"
#include "C2FFMPEGAudioDecodeComponent.h"
#include "C2FFMPEGAudioDecodeInterface.h"
#include "C2FFMPEGVideoDecodeComponent.h"
#include "C2FFMPEGVideoDecodeInterface.h"

namespace android {

// This is the absolute on-device path of the prebuild_etc module
// "android.hardware.media.c2@1.1-ffmpeg-seccomp_policy" in Android.bp.
static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2@1.2-ffmpeg.policy";

// Additional seccomp permissions can be added in this file.
// This file does not exist by default.
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2@1.2-ffmpeg-extended.policy";

static const C2FFMPEGComponentInfo kFFMPEGVideoComponents[] = {
    { "c2.ffmpeg.divx.decoder"  , MEDIA_MIMETYPE_VIDEO_DIVX  , AV_CODEC_ID_MPEG4      },
    { "c2.ffmpeg.flv1.decoder"  , MEDIA_MIMETYPE_VIDEO_FLV1  , AV_CODEC_ID_FLV1       },
    { "c2.ffmpeg.h263.decoder"  , MEDIA_MIMETYPE_VIDEO_H263  , AV_CODEC_ID_H263       },
    { "c2.ffmpeg.hevc.decoder"  , MEDIA_MIMETYPE_VIDEO_HEVC  , AV_CODEC_ID_HEVC       },
    { "c2.ffmpeg.h264.decoder"  , MEDIA_MIMETYPE_VIDEO_AVC   , AV_CODEC_ID_H264       },
    { "c2.ffmpeg.mpeg2.decoder" , MEDIA_MIMETYPE_VIDEO_MPEG2 , AV_CODEC_ID_MPEG2VIDEO },
    { "c2.ffmpeg.mpeg4.decoder" , MEDIA_MIMETYPE_VIDEO_MPEG4 , AV_CODEC_ID_MPEG4      },
    { "c2.ffmpeg.rv.decoder"    , MEDIA_MIMETYPE_VIDEO_RV    , AV_CODEC_ID_RV40       },
    { "c2.ffmpeg.vc1.decoder"   , MEDIA_MIMETYPE_VIDEO_VC1   , AV_CODEC_ID_VC1        },
    { "c2.ffmpeg.vp8.decoder"   , MEDIA_MIMETYPE_VIDEO_VP8   , AV_CODEC_ID_VP8        },
    { "c2.ffmpeg.vp9.decoder"   , MEDIA_MIMETYPE_VIDEO_VP9   , AV_CODEC_ID_VP9        },
    { "c2.ffmpeg.vtrial.decoder", MEDIA_MIMETYPE_VIDEO_FFMPEG, AV_CODEC_ID_NONE       },
    { "c2.ffmpeg.wmv.decoder"   , MEDIA_MIMETYPE_VIDEO_WMV   , AV_CODEC_ID_WMV2       },
};

static const size_t kNumVideoComponents =
    (sizeof(kFFMPEGVideoComponents) / sizeof(kFFMPEGVideoComponents[0]));

static const C2FFMPEGComponentInfo kFFMPEGAudioComponents[] = {
    { "c2.ffmpeg.aac.decoder"   , MEDIA_MIMETYPE_AUDIO_AAC          , AV_CODEC_ID_AAC    },
    { "c2.ffmpeg.ac3.decoder"   , MEDIA_MIMETYPE_AUDIO_AC3          , AV_CODEC_ID_AC3    },
    { "c2.ffmpeg.alac.decoder"  , MEDIA_MIMETYPE_AUDIO_ALAC         , AV_CODEC_ID_ALAC   },
    { "c2.ffmpeg.ape.decoder"   , MEDIA_MIMETYPE_AUDIO_APE          , AV_CODEC_ID_APE    },
    { "c2.ffmpeg.atrial.decoder", MEDIA_MIMETYPE_AUDIO_FFMPEG       , AV_CODEC_ID_NONE   },
    { "c2.ffmpeg.dts.decoder"   , MEDIA_MIMETYPE_AUDIO_DTS          , AV_CODEC_ID_DTS    },
    { "c2.ffmpeg.flac.decoder"  , MEDIA_MIMETYPE_AUDIO_FLAC         , AV_CODEC_ID_FLAC   },
    { "c2.ffmpeg.mp2.decoder"   , MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II, AV_CODEC_ID_MP2    },
    { "c2.ffmpeg.mp3.decoder"   , MEDIA_MIMETYPE_AUDIO_MPEG         , AV_CODEC_ID_MP3    },
    { "c2.ffmpeg.ra.decoder"    , MEDIA_MIMETYPE_AUDIO_RA           , AV_CODEC_ID_COOK   },
    { "c2.ffmpeg.vorbis.decoder", MEDIA_MIMETYPE_AUDIO_VORBIS       , AV_CODEC_ID_VORBIS },
    { "c2.ffmpeg.wma.decoder"   , MEDIA_MIMETYPE_AUDIO_WMA          , AV_CODEC_ID_WMAV2  },
};

static const size_t kNumAudioComponents =
    (sizeof(kFFMPEGAudioComponents) / sizeof(kFFMPEGAudioComponents[0]));

class StoreImpl : public C2ComponentStore {
public:
    StoreImpl()
        : mReflectorHelper(std::make_shared<C2ReflectorHelper>()),
          mInterface(mReflectorHelper) {
    }

    virtual ~StoreImpl() override = default;

    virtual C2String getName() const override {
        return "ffmpeg";
    }

    virtual c2_status_t createComponent(
            C2String name,
            std::shared_ptr<C2Component>* const component) override {
        ALOGD("createComponent: %s", name.c_str());
        for (int i = 0; i < kNumAudioComponents; i++) {
            auto info = &kFFMPEGAudioComponents[i];
            if (name == info->name) {
                component->reset();
                *component = std::shared_ptr<C2Component>(
                        new C2FFMPEGAudioDecodeComponent(
                                info, std::make_shared<C2FFMPEGAudioDecodeInterface>(info, mReflectorHelper)));
                return C2_OK;
            }
        }
        for (int i = 0; i < kNumVideoComponents; i++) {
            auto info = &kFFMPEGVideoComponents[i];
            if (name == info->name) {
                component->reset();
                *component = std::shared_ptr<C2Component>(
                        new C2FFMPEGVideoDecodeComponent(
                                info, std::make_shared<C2FFMPEGVideoDecodeInterface>(info, mReflectorHelper)));
                return C2_OK;
            }
        }
        return C2_NOT_FOUND;
    }

    virtual c2_status_t createInterface(
            C2String name,
            std::shared_ptr<C2ComponentInterface>* const interface) override {
        ALOGD("createInterface: %s", name.c_str());
        for (int i = 0; i < kNumAudioComponents; i++) {
            auto info = &kFFMPEGAudioComponents[i];
            if (name == info->name) {
                interface->reset();
                *interface = std::shared_ptr<C2ComponentInterface>(
                        new SimpleInterface<C2FFMPEGAudioDecodeInterface>(
                                info->name, 0, std::make_shared<C2FFMPEGAudioDecodeInterface>(info, mReflectorHelper)));
                return C2_OK;
            }
        }
        for (int i = 0; i < kNumVideoComponents; i++) {
            auto info = &kFFMPEGVideoComponents[i];
            if (name == info->name) {
                interface->reset();
                *interface = std::shared_ptr<C2ComponentInterface>(
                        new SimpleInterface<C2FFMPEGVideoDecodeInterface>(
                                info->name, 0, std::make_shared<C2FFMPEGVideoDecodeInterface>(info, mReflectorHelper)));
                return C2_OK;
            }
        }
        ALOGE("createInterface: unknown component = %s", name.c_str());
        return C2_NOT_FOUND;
    }

    virtual std::vector<std::shared_ptr<const C2Component::Traits>>
            listComponents() override {
        std::vector<std::shared_ptr<const C2Component::Traits>> ret;
        // FIXME: Prefer OMX codecs for the time being...
        uint32_t defaultRank = ::android::base::GetUintProperty("debug.ffmpeg-codec2.rank", 0x110u);
        uint32_t defaultRankAudio = ::android::base::GetUintProperty("debug.ffmpeg-codec2.rank.audio", defaultRank);
        uint32_t defaultRankVideo = ::android::base::GetUintProperty("debug.ffmpeg-codec2.rank.video", defaultRank);
        ALOGD("listComponents: defaultRank=%x, defaultRankAudio=%x, defaultRankVideo=%x",
              defaultRank, defaultRankAudio, defaultRankVideo);
#define RANK_DISABLED 0xFFFFFFFF
        if (defaultRank != RANK_DISABLED) {
            if (defaultRankAudio != RANK_DISABLED) {
                for (int i = 0; i < kNumAudioComponents; i++) {
                    auto traits = std::make_shared<C2Component::Traits>();
                    traits->name = kFFMPEGAudioComponents[i].name;
                    traits->domain = C2Component::DOMAIN_AUDIO;
                    traits->kind = C2Component::KIND_DECODER;
                    traits->mediaType = kFFMPEGAudioComponents[i].mediaType;
                    traits->rank = defaultRankAudio;
                    ret.push_back(traits);
                }
            }
            if (defaultRankVideo != RANK_DISABLED) {
                for (int i = 0; i < kNumVideoComponents; i++) {
                    auto traits = std::make_shared<C2Component::Traits>();
                    traits->name = kFFMPEGVideoComponents[i].name;
                    traits->domain = C2Component::DOMAIN_VIDEO;
                    traits->kind = C2Component::KIND_DECODER;
                    traits->mediaType = kFFMPEGVideoComponents[i].mediaType;
                    traits->rank = defaultRankVideo;
                    ret.push_back(traits);
                }
            }
        }
        return ret;
    }

    virtual c2_status_t copyBuffer(
            std::shared_ptr<C2GraphicBuffer> /* src */,
            std::shared_ptr<C2GraphicBuffer> /* dst */) override {
        return C2_OMITTED;
    }

    virtual c2_status_t query_sm(
        const std::vector<C2Param*>& stackParams,
        const std::vector<C2Param::Index>& heapParamIndices,
        std::vector<std::unique_ptr<C2Param>>* const heapParams) const override {
        return mInterface.query(stackParams, heapParamIndices, C2_MAY_BLOCK, heapParams);
    }

    virtual c2_status_t config_sm(
            const std::vector<C2Param*>& params,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures) override {
        return mInterface.config(params, C2_MAY_BLOCK, failures);
    }

    virtual std::shared_ptr<C2ParamReflector> getParamReflector() const override {
        return mReflectorHelper;
    }

    virtual c2_status_t querySupportedParams_nb(
            std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const override {
        return mInterface.querySupportedParams(params);
    }

    virtual c2_status_t querySupportedValues_sm(
            std::vector<C2FieldSupportedValuesQuery>& fields) const override {
        return mInterface.querySupportedValues(fields, C2_MAY_BLOCK);
    }

private:
    class Interface : public C2InterfaceHelper {
    public:
        Interface(const std::shared_ptr<C2ReflectorHelper> &helper)
            : C2InterfaceHelper(helper) {
            setDerivedInstance(this);

            addParameter(
                DefineParam(mIonUsageInfo, "ion-usage")
                .withDefault(new C2StoreIonUsageInfo())
                .withFields({
                    C2F(mIonUsageInfo, usage).flags(
                            {C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE}),
                    C2F(mIonUsageInfo, capacity).inRange(0, UINT32_MAX, 1024),
                    C2F(mIonUsageInfo, heapMask).any(),
                    C2F(mIonUsageInfo, allocFlags).flags({}),
                    C2F(mIonUsageInfo, minAlignment).equalTo(0)
                })
                .withSetter(SetIonUsage)
                .build());

            addParameter(
                DefineParam(mDmaBufUsageInfo, "dmabuf-usage")
                .withDefault(C2StoreDmaBufUsageInfo::AllocUnique(0))
                .withFields({
                    C2F(mDmaBufUsageInfo, m.usage).flags({C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE}),
                    C2F(mDmaBufUsageInfo, m.capacity).inRange(0, UINT32_MAX, 1024),
                    C2F(mDmaBufUsageInfo, m.heapName).any(),
                    C2F(mDmaBufUsageInfo, m.allocFlags).flags({}),
                })
                .withSetter(SetDmaBufUsage)
                .build());
        }

        virtual ~Interface() = default;

    private:
        static C2R SetIonUsage(bool /* mayBlock */, C2P<C2StoreIonUsageInfo> &me) {
            // Vendor's TODO: put appropriate mapping logic
            me.set().heapMask = ~0;
            me.set().allocFlags = 0;
            me.set().minAlignment = 0;
            return C2R::Ok();
        }

        static C2R SetDmaBufUsage(bool /* mayBlock */, C2P<C2StoreDmaBufUsageInfo> &me) {
            // Vendor's TODO: put appropriate mapping logic
            strncpy(me.set().m.heapName, "system", me.v.flexCount());
            me.set().m.allocFlags = 0;
            return C2R::Ok();
        }


        std::shared_ptr<C2StoreIonUsageInfo> mIonUsageInfo;
        std::shared_ptr<C2StoreDmaBufUsageInfo> mDmaBufUsageInfo;
    };
    std::shared_ptr<C2ReflectorHelper> mReflectorHelper;
    Interface mInterface;
};

} // namespace android

int main(int /* argc */, char** /* argv */) {
    using namespace ::android;
    LOG(DEBUG) << "android.hardware.media.c2@1.2-service starting...";

    // Set up minijail to limit system calls.
    signal(SIGPIPE, SIG_IGN);
    SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

    ProcessState::self()->startThreadPool();
    // Extra threads may be needed to handle a stacked IPC sequence that
    // contains alternating binder and hwbinder calls. (See b/35283480.)
    hardware::configureRpcThreadpool(8, true /* callerWillJoin */);

    // Create IComponentStore service.
    {
        using namespace ::android::hardware::media::c2::V1_2;
        sp<IComponentStore> store;

        // TODO: Replace this with
        // store = new utils::ComponentStore(
        //         /* implementation of C2ComponentStore */);
        LOG(DEBUG) << "Instantiating Codec2's IComponentStore service...";
        store = new utils::ComponentStore(
                std::make_shared<StoreImpl>());

        if (store == nullptr) {
            LOG(ERROR) << "Cannot create Codec2's IComponentStore service.";
        } else {
            constexpr char const* serviceName = "ffmpeg";
            if (store->registerAsService(serviceName) != OK) {
                LOG(ERROR) << "Cannot register Codec2's IComponentStore service"
                              " with instance name << \""
                           << serviceName << "\".";
            } else {
                LOG(DEBUG) << "Codec2's IComponentStore service registered. "
                              "Instance name: \"" << serviceName << "\".";
            }
        }
    }

    hardware::joinRpcThreadpool();
    return 0;
}
