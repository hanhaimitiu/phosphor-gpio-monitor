// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2016 IBM Corporation

#include "monitor.hpp"

#include <fcntl.h>

#include <phosphor-logging/lg2.hpp>

namespace phosphor
{
namespace gpio
{

// systemd service to kick start a target.
// 用于启动 target 的 systemd 服务。
constexpr auto SYSTEMD_SERVICE = "org.freedesktop.systemd1";
constexpr auto SYSTEMD_ROOT = "/org/freedesktop/systemd1";
constexpr auto SYSTEMD_INTERFACE = "org.freedesktop.systemd1.Manager";

// Callback handler when there is an activity on the FD
// FD 上有活动时的回调处理函数
int Monitor::processEvents(sd_event_source*, int, uint32_t, void* userData)
{
    lg2::info("GPIO line altered");
    auto monitor = static_cast<Monitor*>(userData);

    monitor->analyzeEvent();
    return 0;
}

// Analyzes the GPIO event
// 分析 GPIO 事件
void Monitor::analyzeEvent()
{
    // Data returned
    // 返回的数据
    struct input_event ev{};
    int rc = 0;

    // While testing, observed that not having a loop here was leading
    // into events being missed.
    // 测试时发现，如果这里没有循环，
    // 会导致事件被漏掉。
    while (rc >= 0)
    {
        // Wait until no more events are available on the device.
        // 一直等待，直到设备上不再有可用事件。
        rc = libevdev_next_event(devicePtr.get(), LIBEVDEV_READ_FLAG_NORMAL,
                                 &ev);
        if (rc < 0)
        {
            // There was an error waiting for events, mostly that there are no
            // events to be read.. So continue waiting...
            // 等待事件时出错，通常是因为没有
            // 事件可读，所以继续等待……
            return;
        };

        if (rc == LIBEVDEV_READ_STATUS_SUCCESS)
        {
            if (ev.type == EV_SYN && ev.code == SYN_REPORT)
            {
                continue;
            }
            else if (ev.code == key && ev.value == polarity)
            {
                // If the code/value is what we are interested in, declare done.
                // User supplied systemd unit
                // 如果 code/value 是我们关心的，则宣告完成。
                // 用户提供的 systemd unit
                if (!target.empty())
                {
                    auto bus = sdbusplus::bus::new_default();
                    auto method =
                        bus.new_method_call(SYSTEMD_SERVICE, SYSTEMD_ROOT,
                                            SYSTEMD_INTERFACE, "StartUnit");
                    method.append(target);
                    method.append("replace");

                    bus.call_noreply(method);
                }

                if (!continueAfterKeyPress)
                {
                    // This marks the completion of handling the gpio assertion
                    // and the app can exit
                    // 这标志着 GPIO 置位处理完成，
                    // 应用可以退出
                    complete = true;
                }
                return;
            }
        }
    };

    return;
}

} // namespace gpio
} // namespace phosphor
