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

#ifndef C2_FFMPEG_COMPONENT_INTERFACE_H
#define C2_FFMPEG_COMPONENT_INTERFACE_H

#include <util/C2InterfaceHelper.h>
#include <C2Config.h>

namespace android {

class C2FFMPEGComponentInterface : public C2InterfaceHelper {
public:
    C2FFMPEGComponentInterface(const std::shared_ptr<C2ReflectorHelper> &helper);
    virtual ~C2FFMPEGComponentInterface();

private:
    static C2R SetIonUsage(bool /* mayBlock */, C2P<C2StoreIonUsageInfo> &me);
    static C2R SetDmaBufUsage(bool /* mayBlock */, C2P<C2StoreDmaBufUsageInfo> &me);
    std::shared_ptr<C2StoreIonUsageInfo> mIonUsageInfo;
    std::shared_ptr<C2StoreDmaBufUsageInfo> mDmaBufUsageInfo;
};

} // namespace android

#endif // C2_FFMPEG_COMPONENT_INTERFACE_H
