/*
 * Copyright (C) 2016 The Android Open Source Project
 * Copyright (C) 2023 The LineageOS Project
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

#include <csignal>
#include <fstream>
#include <poll.h>
#include <unistd.h>

#include "HdmiCec.h"

#include <linux/cec.h>
#include <misc/tegra_cec.h>
#include <video/tegra_dc_ext.h>

using ::android::hardware::tv::cec::V1_0::HdmiPortType;

namespace android {
namespace hardware {
namespace tv {
namespace cec {
namespace V1_0 {
namespace implementation {

sp<IHdmiCecCallback> HdmiCec::mCallback = nullptr;

// Private methods
HdmiCec::HdmiCec() {
    dcctrl_thread = std::thread(&HdmiCec::dcctrl_worker, this);

    // Tegra cec driver doesn't properly support polling and non-blocking reads,
    // so a signal has to be sent to the cecdev thread to break the blocking read
    // to shut down the thread.
    std::signal(SIGINT, [](int) {});
    cecdev = open("/dev/tegra_cec", O_RDWR);
    cecdev_thread = std::thread(&HdmiCec::cecdev_worker, this);
}

HdmiCec::~HdmiCec() {
    dcctrl_stop = true;
    if (dcctrl_thread.joinable())
        dcctrl_thread.join();

    cecdev_stop = true;
    pthread_kill(cecdev_thread.native_handle(), SIGINT);
    if (cecdev_thread.joinable())
        cecdev_thread.join();

    if (cecdev >= 0)
        close(cecdev);
}

/*
 * Per the hdmi 1.4 spec, "00 0C 03" is a vendor specific data block for hdmi 1.4 info.
 * The two bytes after the identifier are the physical address assigned to the device.
 */
void HdmiCec::get_physical_address(int dcctrl, uint8_t dispid) {
    char edid[512];
    struct tegra_dc_ext_control_output_edid dcedid = { dispid, sizeof(edid), edid };

    if (ioctl(dcctrl, TEGRA_DC_EXT_CONTROL_GET_OUTPUT_EDID, &dcedid) == 0) {
        for (uint16_t i = 0; i < (dcedid.size - 5); i++) {
            if (edid[i] == 3 && edid[i+1] == 12 && edid[i+2] == 0) {
                mPhysAddrMutex.lock();
                mPhysAddr = edid[i+3] << 8 | edid[i+4];
                mPhysAddrMutex.unlock();
                break;
            }
        }
    }
}

/*
 * Tegra cec manages logicial addresses via a sysfs node, not via ioctl.
 * It prints addresses in hex format, but uses functions that assume decimal for write.
 */
bool HdmiCec::cec_configure_logical_addr(int32_t new_addr) {
    std::fstream laddr("/sys/devices/platform/tegra_cec/cec_logical_addr_config");

    if (!laddr.is_open())
        return false;

    // An address of 0 is a request to clear all addresses
    if (new_addr == 0) {
        laddr << 0 << std::endl;
        return true;
    }

    // Get current logical addresses, then add the new one
    int32_t addrs = 0;
    laddr >> std::hex >> addrs;
    addrs |= new_addr;
    laddr << std::dec << (addrs & 0x7FFF) << std::endl;

    return true;
}

/*
 * Main function for accessing tegra dc ioctls.
 * The file descriptor to dcctrl is local here, thus no mutexes are
 * necessary to guard access.
 */
void HdmiCec::dcctrl_worker() {
    int dcctrl = open("/dev/tegra_dc_ctrl", O_RDONLY | O_NONBLOCK);
    uint8_t dispid = 0;

    // If the node cannot be opened, just bail
    if (dcctrl < 0)
        return;

    // Look up the number of available heads
    uint32_t numdc = 0, i = 0;
    if (ioctl(dcctrl, TEGRA_DC_EXT_CONTROL_GET_NUM_OUTPUTS, &numdc) < 0)
        numdc = 0;

    // Loop through heads looking for the primary hdmi head, then
    // check to see if a display is connected
    struct tegra_dc_ext_control_output_properties dcprops;
    for (i = 0; i < numdc; i++) {
        dcprops.handle = i;
        if (ioctl(dcctrl, TEGRA_DC_EXT_CONTROL_GET_OUTPUT_PROPERTIES, &dcprops) == 0 &&
            dcprops.type == TEGRA_DC_EXT_HDMI) {
            dispid = i;
            mConnectedMutex.lock();
            mConnected = dcprops.connected;
            mConnectedMutex.unlock();
            break;
        }
    }

    // If an hdmi head was not found, just bail
    if (i == numdc) {
        close(dcctrl);
        return;
    }

    // If a display is available, get the assigned physical address
    mConnectedMutex.lock();
    if (mConnected)
        get_physical_address(dcctrl, dispid);
    mConnectedMutex.unlock();

    // Tell tegradc to notify us if a hotplug event occurs
    ioctl(dcctrl, TEGRA_DC_EXT_CONTROL_SET_EVENT_MASK, TEGRA_DC_EXT_EVENT_HOTPLUG);

    // Poll, waiting on hotplug events
    struct pollfd fds[1] = { {dcctrl, POLLIN, 0} };
    int pollret = 0;
    while (!dcctrl_stop.load()) {
        pollret = poll(fds, 1, 100);
        if (pollret == -1)
            break;

        if (pollret > 0 && (fds[0].revents & POLLIN)) {
            struct tegra_dc_ext_event dcevent;
            if (read(dcctrl, &dcevent, sizeof(dcevent)) > 0 &&
                dcevent.type == TEGRA_DC_EXT_EVENT_HOTPLUG) {
                struct tegra_dc_ext_control_event_hotplug dchotplug;
                if (read(dcctrl, &dchotplug, sizeof(dchotplug)) > 0 &&
                    dchotplug.handle == dispid) {
                    mConnectedMutex.lock();
                    dcprops.handle = dchotplug.handle;
                    if (ioctl(dcctrl, TEGRA_DC_EXT_CONTROL_GET_OUTPUT_PROPERTIES, &dcprops) == 0)
                        mConnected = dcprops.connected;

                    // If display connected, get new physical address
                    // If display disconnected, clear all logical addresses
                    if (mConnected)
                        get_physical_address(dcctrl, dispid);
                    else
                        cec_configure_logical_addr(0);

                    // Notify frameworks that a hotplug event occured
                    if (mCallback != nullptr) {
                        HotplugEvent hotplugEvent{
                                .connected = mConnected,
                                .portId = 1 };
                        mCallback->onHotplugEvent(hotplugEvent);
                    }
                    mConnectedMutex.unlock();
                }
            }
        }
    }

    if (dcctrl >= 0)
        close(dcctrl);
}

/*
 * Per commit message in kernel/nvidia change id Ia3835cec0bb717e63dabca5c5fcb1236847bf492
 *  READ API:
 *          read API ignores count and will always return 16 bit data.
 *          read API expects user to supply it with min of 16 bits data
 *          it returns CEC packet in following format
 *          bit 0-7: data
 *          bit 8: EOM
 *          bit 9: ACK
 */
void HdmiCec::cecdev_worker() {
    // If the node failed to open, just bail
    if (cecdev < 0)
        return;

    // Blocking loop, waiting on incoming cec messages
    while (!cecdev_stop.load()) {
        CecMessage cecMessage;
        uint16_t cec_temp;
        if (read(cecdev, &cec_temp, sizeof(cec_temp)) > 0) {
            // This is the header, set from/to
            cecMessage.initiator = static_cast<CecLogicalAddress>((cec_temp & 0xF0) >> 4);
            cecMessage.destination = static_cast<CecLogicalAddress>(cec_temp & 0xF);

            // Loop reading message body until driver signals end of message
            while (cecMessage.body.size() <= static_cast<size_t>(MaxLength::MESSAGE_BODY) &&
                   (cec_temp & 0x0100) == 0 &&
                   read(cecdev, &cec_temp, sizeof(cec_temp)) == 2) {
                cecMessage.body.resize(cecMessage.body.size()+1);
                cecMessage.body[cecMessage.body.size()-1] = cec_temp & 0xFF;
            }

            // If EOM bit not set, assume error and request driver reset
            if ((cec_temp & 0x0100) == 0) {
                if (ioctl(cecdev, TEGRA_CEC_IOCTL_ERROR_RECOVERY) < 0)
                    break;

                continue;
            }

            // Notifiy frameworks of received message
            if (mCallback != nullptr)
                mCallback->onCecMessage(cecMessage);
        }
    }
}

// Methods from ::android::hardware::tv::cec::V1_0::IHdmiCec follow.
Return<Result> HdmiCec::addLogicalAddress(CecLogicalAddress addr) {
    if (addr >= CecLogicalAddress::UNREGISTERED)
        return Result::FAILURE_INVALID_ARGS;

    // Tegra cec handles logicial addresses as a bitmask offset
    if (cec_configure_logical_addr(1 << static_cast<int32_t>(addr)))
        return Result::SUCCESS;
    else
        return Result::FAILURE_UNKNOWN;
}

Return<void> HdmiCec::clearLogicalAddress() {
    // This function handles zero as a clear request
    cec_configure_logical_addr(0);
    return Void();
}

/*
 * The physical address is looked up on hal start and on hotplug
 * connected events, so only need to grab the stashed value here.
 */
Return<void> HdmiCec::getPhysicalAddress(getPhysicalAddress_cb _hidl_cb) {
    mPhysAddrMutex.lock();
    if (mPhysAddr == 0xFFFF)
        _hidl_cb(Result::FAILURE_UNKNOWN, mPhysAddr);
    else
        _hidl_cb(Result::SUCCESS, mPhysAddr);
    mPhysAddrMutex.unlock();

    return Void();
}

/*
 * Per commit message in kernel/nvidia change id Iabdd92b5658dd63c7b500a7ec88d79a64c8c0a43
 *  write() API:
 *  -Userspace is responsible for re-transmission.
 *  -Read from user-space byte by byte, each byte representing
 *   a block, up to 16 bytes as specified by HDMI standard.
 *  -Return 0 on success transmission, -1 otherwise, with errno
 *   setup as follows:
 *    EIO - TX_REGISTER_UNDERRUN, should not happen, otherwise
 *        driver is have serious timing issue.
 *    ECOMM - BUS arbitration failure or anomaly BUS activity.
 *        Transmission is abandoned.
 *    ECONNRESET - For broadcast message only, someone on the BUS
 *        asserted NAK during transmission.
 *    EHOSTUNREACH - For direct message only, message was not ACK'd.
 *        (Required by logical address allocation)
 *    EMSGSIZE - Message size > 16 bit.
 *    EFAULT - Page fault accessing message buffer.
 *    EINTR - call interrupted by singal.
 */
Return<SendMessageResult> HdmiCec::sendMessage(const CecMessage& message) {
    // If The message is malformed, respond with NACK
    // If the cecdev node is not open, this is a hard fail
    if (message.body.size() > static_cast<size_t>(MaxLength::MESSAGE_BODY) ||
        message.initiator >= CecLogicalAddress::UNREGISTERED ||
        message.destination > CecLogicalAddress::BROADCAST) {
        return SendMessageResult::NACK;
    } else if (cecdev < 0) {
        return SendMessageResult::FAIL;
    }

    // Copy the message into a vector, so it can be used as a single buffer for write
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(message.initiator) << 4 | static_cast<uint8_t>(message.destination & 0xF));
    msg.insert(msg.end(), message.body.begin(), message.body.end());

    // If write returns an error, report a fail.
    return (write(cecdev, msg.data(), msg.size()) >= 0 ? SendMessageResult::SUCCESS : SendMessageResult::FAIL);
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

/*
 * Driver only supports cec as in the hdmi 1.4 spec
 */
Return<int32_t> HdmiCec::getCecVersion() {
    return CEC_OP_CEC_VERSION_1_4;
}

/*
 * Nvidia vendor id is 0x044B
 */
Return<uint32_t> HdmiCec::getVendorId() {
    return 0x044B;
}

/*
 * Hal only supports a single port, so just return a static list
 */
Return<void> HdmiCec::getPortInfo(getPortInfo_cb _hidl_cb) {
    mPhysAddrMutex.lock();
    hidl_vec<HdmiPortInfo> portInfos {
        { HdmiPortType::OUTPUT, 1, true, false, mPhysAddr }
    };
    mPhysAddrMutex.unlock();
    _hidl_cb(portInfos);
    return Void();
}

/*
 * Options are not currently supported
 */
Return<void> HdmiCec::setOption(__attribute__((unused)) OptionKey key, __attribute__((unused)) bool value) {
    // Unimplemented
    return Void();
}

Return<void> HdmiCec::setLanguage(__attribute__((unused)) const hidl_string& language) {
    // Unimplemented
    return Void();
}

/*
 * Arc not relevant on playback devices
 */
Return<void> HdmiCec::enableAudioReturnChannel(__attribute__((unused)) int32_t portId, __attribute__((unused)) bool enable) {
    // Unimplemented
    return Void();
}

/*
 * The connection status is looked up on hal start and on hotplug
 * connected events, so only need to grab the stashed value here.
 */
Return<bool> HdmiCec::isConnected(__attribute__((unused)) int32_t portId) {
    mConnectedMutex.lock();
    bool ret = mConnected;
    mConnectedMutex.unlock();

    return ret;
}

/*
 * Register the service
 */
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
