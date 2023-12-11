/*
 * Copyright (C) 2017 The Android Open Source Project
 * Copyright (C) 2017-2018 The LineageOS Project
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

#define LOG_TAG "android.hardware.tv.cec@1.0-service.nvidia"

#include <android/log.h>
#include <hidl/HidlTransportSupport.h>
#include "HdmiCec.h"

using android::sp;
using android::status_t;
using android::OK;

// libhwbinder:
using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;

// Generated HIDL files
using android::hardware::tv::cec::V1_0::implementation::HdmiCec;

int main() {

    status_t status;
    android::sp<HdmiCec> service = nullptr;

    ALOGI("HDMI CEC HAL Service 1.0 for Nvidia is starting.");

    service = new HdmiCec();
    if (service == nullptr) {
        ALOGE("Can not create an instance of HDMI CEC HAL Iface, exiting.");

        goto shutdown;
    }

    configureRpcThreadpool(1, true /*callerWillJoin*/);

    status = service->registerAsSystemService();
    if (status != OK) {
        ALOGE("Could not register service for HDMI CEC HAL Iface (%d).", status);
        goto shutdown;
    }

    ALOGI("HDMI CEC Service is ready");
    joinRpcThreadpool();
    //Should not pass this line

shutdown:
    // In normal operation, we don't expect the thread pool to exit

    ALOGE("HDMI CEC Service is shutting down");
    return 1;
}
