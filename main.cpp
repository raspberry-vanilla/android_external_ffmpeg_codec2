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

#define LOG_TAG "android.hardware.media.c2-service-ffmpeg"

#include "C2FFMPEGComponentStore.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <codec2/aidl/ComponentStore.h>
#include <minijail.h>

using namespace ::aidl::android::hardware::media::c2;

// This is the absolute on-device path of the prebuilt_etc module
// "android.hardware.media.c2-ffmpeg-default-seccomp_policy" in Android.bp.
static constexpr char kBaseSeccompPolicyPath[] =
        "/apex/com.android.hardware.media.c2.ffmpeg/etc/seccomp_policy/"
        "android.hardware.media.c2-ffmpeg-default-seccomp_policy";

// Additional seccomp permissions can be added in this file.
static constexpr char kExtSeccompPolicyPath[] =
        "/apex/com.android.hardware.media.c2.ffmpeg/etc/seccomp_policy/"
        "android.hardware.media.c2-ffmpeg-extended-seccomp_policy";

int main() {
    LOG(DEBUG) << "android.hardware.media.c2-service-ffmpeg starting...";

    // Set up minijail to limit system calls.
    signal(SIGPIPE, SIG_IGN);
    android::SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

    // Extra threads may be needed to handle a stacked IPC sequence that
    // contains alternating binder and hwbinder calls. (See b/35283480.)
    ABinderProcess_setThreadPoolMaxThreadCount(8);
    ABinderProcess_startThreadPool();

    // Create IComponentStore service.
    std::shared_ptr<IComponentStore> store = ndk::SharedRefBase::make<utils::ComponentStore>(
            std::make_shared<android::C2FFMPEGComponentStore>());

    const std::string instance = std::string() + IComponentStore::descriptor + "/ffmpeg";
    binder_status_t status = AServiceManager_addService(store->asBinder().get(), instance.c_str());
    CHECK(status == STATUS_OK);

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should not reach
}
