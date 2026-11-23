// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#include "evdev.hpp"

#include "xyz/openbmc_project/Common/error.hpp"

#include <fcntl.h>
#include <libevdev/libevdev.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>

namespace phosphor
{
namespace gpio
{

using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Common::Error;

// Populate the file descriptor for passed in device
// 为传入的设备填充文件描述符
int Evdev::openDevice()
{
    auto fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
    {
        lg2::error("Failed to open {DEVICEPATH}: {ERRNO}", "DEVICEPATH", path,
                   "ERRNO", errno);
        elog<InternalFailure>();
    }
    return fd;
}

// Initializes the event device with the fd
// 用该 fd 初始化事件设备
void Evdev::initEvDev()
{
    if (devicePtr)
    {
        // Init can be done only once per device
        // 每个设备只能初始化一次
        return;
    }

    struct libevdev* evdev = nullptr;
    auto rc = libevdev_new_from_fd((fd)(), &evdev);
    if (rc < 0)
    {
        lg2::error("Failed to initialize evdev");
        elog<InternalFailure>();
        return;
    }

    // Packing in the unique_ptr
    // 封装进 unique_ptr
    devicePtr.reset(evdev);
}

// Attaches the FD to event loop and registers the callback handler
// 将 FD 附加到事件循环并注册回调处理函数
void Evdev::registerCallback()
{
    decltype(eventSource.get()) sourcePtr = nullptr;
    auto rc = sd_event_add_io(event.get(), &sourcePtr, (fd)(), EPOLLIN,
                              callbackHandler, this);
    eventSource.reset(sourcePtr);

    if (rc < 0)
    {
        lg2::error("Failed to register callback handler: {RC}", "RC", rc);
        elog<InternalFailure>();
    }
}

} // namespace gpio
} // namespace phosphor
