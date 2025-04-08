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

#include "C2FFMPEGComponentInterface.h"

namespace android {

C2FFMPEGComponentInterface::C2FFMPEGComponentInterface(const std::shared_ptr<C2ReflectorHelper> &helper)
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
            C2F(mDmaBufUsageInfo, m.allocFlags).flags({}),
            C2F(mDmaBufUsageInfo, m.heapName).any(),
        })
        .withSetter(SetDmaBufUsage)
        .build());
}

C2FFMPEGComponentInterface::~C2FFMPEGComponentInterface() = default;

C2R C2FFMPEGComponentInterface::SetIonUsage(bool /* mayBlock */, C2P<C2StoreIonUsageInfo> &me) {
    // Vendor's TODO: put appropriate mapping logic
    me.set().heapMask = ~0;
    me.set().allocFlags = 0;
    me.set().minAlignment = 0;
    return C2R::Ok();
}

C2R C2FFMPEGComponentInterface::SetDmaBufUsage(bool /* mayBlock */, C2P<C2StoreDmaBufUsageInfo> &me) {
    // Vendor's TODO: put appropriate mapping logic
    strncpy(me.set().m.heapName, "system", me.v.flexCount());
    me.set().m.allocFlags = 0;
    return C2R::Ok();
}

} // namespace android
