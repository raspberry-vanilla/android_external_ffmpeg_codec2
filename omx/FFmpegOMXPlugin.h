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

#ifndef FFMPEG_OMX_PLUGIN_H_

#define FFMPEG_OMX_PLUGIN_H_

#include <media/hardware/OMXPluginBase.h>
#include <media/stagefright/foundation/ABase.h>

#include <OMX_Core.h>

#include "config.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"

namespace android {

struct FFmpegOMXPlugin : public OMXPluginBase {
    FFmpegOMXPlugin();

    virtual OMX_ERRORTYPE makeComponentInstance(
            const char *name,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component);

    virtual OMX_ERRORTYPE destroyComponentInstance(
            OMX_COMPONENTTYPE *component);

    virtual OMX_ERRORTYPE enumerateComponents(
            OMX_STRING name,
            size_t size,
            OMX_U32 index);

    virtual OMX_ERRORTYPE getRolesOfComponent(
            const char *name,
            Vector<String8> *roles);

private:

    typedef OMX_ERRORTYPE (*ComponentNameEnumFunc)(
            OMX_STRING, OMX_U32, OMX_U32);

    typedef OMX_ERRORTYPE (*GetHandleFunc)(
            OMX_HANDLETYPE *, OMX_STRING, OMX_PTR, OMX_CALLBACKTYPE *);

    typedef OMX_ERRORTYPE (*FreeHandleFunc)(OMX_HANDLETYPE *);

    typedef OMX_ERRORTYPE (*GetRolesOfComponentFunc)(
            OMX_STRING, OMX_U32 *, OMX_U8 **);

    DISALLOW_EVIL_CONSTRUCTORS(FFmpegOMXPlugin);
};

}  // namespace android

#endif  // SOFT_OMX_PLUGIN_H_
