/*
 * Copyright (C) 2016 The Android Open Source Project
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
#include <utils/Log.h>

#include "HdmiCec.h"

namespace android {
namespace hardware {
namespace tv {
namespace cec {
namespace V1_0 {
namespace implementation {

sp<IHdmiCecCallback> HdmiCec::mCallback = nullptr;

// Methods from ::android::hardware::tv::cec::V1_0::IHdmiCec follow.
Return<Result> HdmiCec::addLogicalAddress(__attribute__((unused)) CecLogicalAddress addr) {
    return Result::FAILURE_UNKNOWN;
}

Return<void> HdmiCec::clearLogicalAddress() {
    return Void();
}

Return<void> HdmiCec::getPhysicalAddress(getPhysicalAddress_cb _hidl_cb) {
    _hidl_cb(Result::FAILURE_UNKNOWN, 0);

    return Void();
}

Return<SendMessageResult> HdmiCec::sendMessage(__attribute__((unused)) const CecMessage& message) {
    return SendMessageResult::FAILURE_UNKNOWN;
}

Return<void> HdmiCec::setCallback(const sp<IHdmiCecCallback>& callback) {
    if (mCallback != nullptr) {
        mCallback->unlinkToDeath(this);
        mCallback = nullptr;
    }

    if (callback != nullptr) {
        mCallback = callback;
        mCallback->linkToDeath(this, 0 /*cookie*/);
    }
    return Void();
}

Return<int32_t> HdmiCec::getCecVersion() {
    return -1;
}

Return<uint32_t> HdmiCec::getVendorId() {
    return 0;
}

Return<void> HdmiCec::getPortInfo(getPortInfo_cb _hidl_cb) {
    mPhysAddrMutex.lock();
    hidl_vec<HdmiPortInfo> portInfos {
        { HdmiPortType::OUTPUT, 1, false, false, 0 }
    };
    mPhysAddrMutex.unlock();
    _hidl_cb(portInfos);
    return Void();
}

Return<void> HdmiCec::setOption(__attribute__((unused)) OptionKey key, __attribute__((unused)) bool value) {
    return Void();
}

Return<void> HdmiCec::setLanguage(__attribute__((unused)) const hidl_string& language) {
    return Void();
}

Return<void> HdmiCec::enableAudioReturnChannel(__attribute__((unused)) int32_t portId, __attribute__((unused)) bool enable) {
    return Void();
}

Return<bool> HdmiCec::isConnected(__attribute__((unused)) int32_t portId) {
    return false;
}

status_t HdmiCec::registerAsSystemService() {
    status_t ret = 0;

    ret = IHdmiCec::registerAsService();
    if (ret != 0) {
        ALOGE("Failed to register IHdmiCec (%d)", ret);
        goto fail;
    } else {
        ALOGI("Successfully registered IHdmiCec");
    }

fail:
    return ret;
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace cec
}  // namespace tv
}  // namespace hardware
}  // namespace android
