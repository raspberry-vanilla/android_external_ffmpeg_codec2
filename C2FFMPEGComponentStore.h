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

#ifndef C2_FFMPEG_COMPONENT_STORE_H
#define C2_FFMPEG_COMPONENT_STORE_H

#include "C2FFMPEGComponentInterface.h"

namespace android {

class C2FFMPEGComponentStore : public C2ComponentStore {
public:
    C2FFMPEGComponentStore();
    virtual ~C2FFMPEGComponentStore() override;

    virtual C2String getName() const override;
    virtual c2_status_t createComponent(
            C2String name,
            std::shared_ptr<C2Component>* const component) override;
    virtual c2_status_t createInterface(
            C2String name,
            std::shared_ptr<C2ComponentInterface>* const interface) override;
    virtual std::vector<std::shared_ptr<const C2Component::Traits>>
            listComponents() override;
    virtual c2_status_t copyBuffer(
            std::shared_ptr<C2GraphicBuffer> /* src */,
            std::shared_ptr<C2GraphicBuffer> /* dst */) override;
    virtual c2_status_t query_sm(
            const std::vector<C2Param*>& stackParams,
            const std::vector<C2Param::Index>& heapParamIndices,
            std::vector<std::unique_ptr<C2Param>>* const heapParams) const override;
    virtual c2_status_t config_sm(
            const std::vector<C2Param*>& params,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures) override;
    virtual std::shared_ptr<C2ParamReflector> getParamReflector() const override;
    virtual c2_status_t querySupportedParams_nb(
            std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const override;
    virtual c2_status_t querySupportedValues_sm(
            std::vector<C2FieldSupportedValuesQuery>& fields) const override;

private:
    std::shared_ptr<C2ReflectorHelper> mReflectorHelper;
    C2FFMPEGComponentInterface mInterface;
};

} // namespace android

#endif // C2_FFMPEG_COMPONENT_STORE_H
