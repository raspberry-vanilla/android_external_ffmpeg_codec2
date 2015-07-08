/*
 * Copyright (C) 2015 The CyanogenMod Project
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

#define LOG_NDEBUG 0
#define LOG_TAG "FFmpegOMXPlugin"
#include <utils/Log.h>

#include "include/SoftOMXComponent.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AString.h>

#include "FFmpegOMXPlugin.h"
#include "FFmpegComponents.h"
#include "SoftFFmpegAudio.h"
#include "SoftFFmpegVideo.h"

namespace android {

OMXPluginBase *createOMXPlugin() {
    return new FFmpegOMXPlugin();
}

FFmpegOMXPlugin::FFmpegOMXPlugin() {
}

OMX_ERRORTYPE FFmpegOMXPlugin::makeComponentInstance(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component) {

    ALOGV("makeComponentInstance '%s'", name);

    sp<SoftOMXComponent> codec;

    for (size_t i = 0; i < kNumAudioComponents; ++i) {
        if (!strcasecmp(name, android::kAudioComponents[i].mName)) {
            codec = SoftFFmpegAudio::createSoftOMXComponent(name, callbacks, appData, component);
            break;
        }
    }

    if (codec == NULL) {
        for (size_t i = 0; i < kNumVideoComponents; ++i) {
            if (!strcasecmp(name, android::kVideoComponents[i].mName)) {
                codec = SoftFFmpegVideo::createSoftOMXComponent(name, callbacks, appData, component);
                break;
            }
        }
    }

    if (codec != NULL) {
        OMX_ERRORTYPE err = codec->initCheck();
        if (err != OMX_ErrorNone) {
            return err;
        }

        codec->incStrong(this);

        return OMX_ErrorNone;
    }

    return OMX_ErrorInvalidComponentName;
}

OMX_ERRORTYPE FFmpegOMXPlugin::destroyComponentInstance(
        OMX_COMPONENTTYPE *component) {
    SoftOMXComponent *me =
        (SoftOMXComponent *)
            ((OMX_COMPONENTTYPE *)component)->pComponentPrivate;

    me->prepareForDestruction();

    void *libHandle = me->libHandle();

    CHECK_EQ(me->getStrongCount(), 1);
    me->decStrong(this);
    me = NULL;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE FFmpegOMXPlugin::enumerateComponents(
        OMX_STRING name,
        size_t /* size */,
        OMX_U32 index) {
    if (index >= kNumComponents) {
        return OMX_ErrorNoMore;
    }

    if (index < kNumAudioComponents) {
        strcpy(name, kAudioComponents[index].mName);
    } else {
        strcpy(name, kVideoComponents[index - kNumAudioComponents].mName);
    }

    return OMX_ErrorNone;
}

OMX_ERRORTYPE FFmpegOMXPlugin::getRolesOfComponent(
        const char *name,
        Vector<String8> *roles) {

    for (size_t i = 0; i < kNumAudioComponents; ++i) {
        if (strcmp(name, kAudioComponents[i].mName)) {
            continue;
        }

        roles->clear();
        roles->push(String8(kAudioComponents[i].mRole));

        return OMX_ErrorNone;
    }

    for (size_t i = 0; i < kNumVideoComponents; ++i) {
        if (strcmp(name, kVideoComponents[i].mName)) {
            continue;
        }

        roles->clear();
        roles->push(String8(kVideoComponents[i].mRole));

        return OMX_ErrorNone;
    }

    return OMX_ErrorInvalidComponentName;
}

}  // namespace android
