// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2016 IBM Corporation

#pragma once

#include "evdev.hpp"

#include <linux/input.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>

#include <string>

namespace phosphor
{
namespace gpio
{

/** @class Monitor
 *  @brief Responsible for catching GPIO state change
 *  condition and starting systemd targets.
 */
/* @class Monitor
 *  @brief 负责捕获 GPIO 状态变化
 *  条件并启动 systemd target。 */
class Monitor : public Evdev
{
  public:
    Monitor() = delete;
    ~Monitor() = default;
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;
    Monitor(Monitor&&) = delete;
    Monitor& operator=(Monitor&&) = delete;

    /** @brief Constructs Monitor object.
     *
     *  @param[in] path     - Path to gpio input device
     *  @param[in] key      - GPIO key to monitor
     *  @param[in] polarity - GPIO assertion polarity to look for
     *  @param[in] target   - systemd unit to be started on GPIO
     *                        value change
     *  @param[in] event    - sd_event handler
     *  @param[in] continueRun - Whether to continue after key pressed
     *  @param[in] handler  - IO callback handler. Defaults to one in this
     *                        class
     *  @param[in] useEvDev - Whether to use EvDev to retrieve events
     */
    /* @brief 构造 Monitor 对象。
     *  @param[in] path     - GPIO 输入设备的路径
     *  @param[in] key      - 要监控的 GPIO key
     *  @param[in] polarity - 要检测的 GPIO 置位极性
     *  @param[in] target   - GPIO 值变化时
     *                        要启动的 systemd unit
     *  @param[in] event    - sd_event 句柄
     *  @param[in] continueRun - 按键之后是否继续
     *  @param[in] handler  - IO 回调处理函数，默认为本
     *                        类中的实现
     *  @param[in] useEvDev - 是否使用 EvDev 来获取事件 */
    Monitor(const std::string& path, decltype(input_event::code) key,
            decltype(input_event::value) polarity, const std::string& target,
            EventPtr& event, bool continueRun,
            sd_event_io_handler_t handler = Monitor::processEvents,
            bool useEvDev = true) :
        Evdev(path, key, event, handler, useEvDev), polarity(polarity),
        target(target), continueAfterKeyPress(continueRun) {};

    /** @brief Callback handler when the FD has some activity on it
     *
     *  @param[in] es       - Populated event source
     *  @param[in] fd       - Associated File descriptor
     *  @param[in] revents  - Type of event
     *  @param[in] userData - User data that was passed during registration
     *
     *  @return             - 0 or positive number on success and negative
     *                        errno otherwise
     */
    /* @brief FD 上有活动时的回调处理函数
     *  @param[in] es       - 已填充的事件源
     *  @param[in] fd       - 关联的文件描述符
     *  @param[in] revents  - 事件类型
     *  @param[in] userData - 注册时传入的用户数据
     *  @return             - 成功返回 0 或正数，否则返回负的
     *                        errno */
    static int processEvents(sd_event_source* es, int fd, uint32_t revents,
                             void* userData);

    /** @brief Returns the completion state of this handler */
    /* @brief 返回该处理函数的完成状态 */
    inline auto completed() const
    {
        return complete;
    }

  private:
    /** @brief GPIO key value that is of interest */
    /* @brief 关心的 GPIO key 值 */
    decltype(input_event::value) polarity;

    /** @brief Systemd unit to be started when the condition is met */
    /* @brief 条件满足时要启动的 systemd unit */
    const std::string& target;

    /** @brief If the monitor should continue after key press */
    /* @brief 按键之后监控器是否应继续 */
    bool continueAfterKeyPress;

    /** @brief Completion indicator */
    /* @brief 完成标志 */
    bool complete = false;

    /** @brief Analyzes the GPIO event and starts configured target */
    /* @brief 分析 GPIO 事件并启动配置的 target */
    void analyzeEvent();
};

} // namespace gpio
} // namespace phosphor
