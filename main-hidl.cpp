/*
 * Copyright (C) 2022 Michael Goffioul <michael.goffioul@gmail.com>
 * Copyright (C) 2025 KonstaKANG
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

#define LOG_TAG "android.hardware.media.c2@1.2-service-ffmpeg"

#include "C2FFMPEGComponentStore.h"

#include <android-base/logging.h>
#include <binder/ProcessState.h>
#include <codec2/hidl/1.2/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>
#include <minijail.h>

using namespace ::android;
using namespace ::android::hardware::media::c2::V1_2;

// This is the absolute on-device path of the prebuild_etc module
// "android.hardware.media.c2-ffmpeg.policy" in Android.bp.
static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-ffmpeg.policy";

// Additional seccomp permissions can be added in this file.
// This file does not exist by default.
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-ffmpeg-extended.policy";

int main() {
    LOG(DEBUG) << "android.hardware.media.c2@1.2-service-ffmpeg starting...";

    // Set up minijail to limit system calls.
    signal(SIGPIPE, SIG_IGN);
    SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

    ProcessState::self()->startThreadPool();
    // Extra threads may be needed to handle a stacked IPC sequence that
    // contains alternating binder and hwbinder calls. (See b/35283480.)
    hardware::configureRpcThreadpool(8, true /* callerWillJoin */);

    // Create IComponentStore service.
    sp<IComponentStore> store;

    // TODO: Replace this with
    // store = new utils::ComponentStore(
    //         /* implementation of C2ComponentStore */);
    LOG(DEBUG) << "Instantiating Codec2's IComponentStore service...";
    store = new utils::ComponentStore(
            std::make_shared<C2FFMPEGComponentStore>());

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

    hardware::joinRpcThreadpool();
    return 0;
}
