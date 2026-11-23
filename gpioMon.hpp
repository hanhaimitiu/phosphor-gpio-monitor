// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#pragma once

#include <gpiod.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#include <map>
#include <vector>

namespace phosphor
{
namespace gpio
{

/** @class GpioMonitor
 *  @brief Responsible for catching GPIO state change
 *  condition and starting systemd targets.
 */
/* @class GpioMonitor
 *  @brief 负责捕获 GPIO 状态变化
 *  条件并启动 systemd target。 */
class GpioMonitor
{
  public:
    GpioMonitor() = delete;
    ~GpioMonitor() = default;
    GpioMonitor(const GpioMonitor&) = delete;
    GpioMonitor& operator=(const GpioMonitor&) = delete;
    GpioMonitor(GpioMonitor&&) = delete;
    GpioMonitor& operator=(GpioMonitor&&) = delete;

    /** @brief Constructs GpioMonitor object.
     *
     *  @param[in] line        - GPIO line from libgpiod
     *  @param[in] config      - configuration of line with event
     *  @param[in] io          - io service
     *  @param[in] target      - systemd unit to be started on GPIO
     *                           value change
     *  @param[in] targets     - systemd units to be started on GPIO
     *                           value change
     *  @param[in] lineMsg     - GPIO line message to be used for log
     *  @param[in] continueRun - Whether to continue after event occur
     */
    /* @brief 构造 GpioMonitor 对象。
     *  @param[in] line        - 来自 libgpiod 的 GPIO line
     *  @param[in] config      - 带事件的 line 配置
     *  @param[in] io          - io 服务
     *  @param[in] target      - GPIO 值变化时
     *                           要启动的 systemd unit
     *  @param[in] targets     - GPIO 值变化时
     *                           要启动的多个 systemd unit
     *  @param[in] lineMsg     - 用于日志的 GPIO line 消息
     *  @param[in] continueRun - 事件发生之后是否继续 */
    GpioMonitor(gpiod_line* line, gpiod_line_request_config& config,
                boost::asio::io_context& io, const std::string& target,
                const std::map<std::string, std::vector<std::string>>& targets,
                const std::string& lineMsg, bool continueRun) :
        gpioLine(line), gpioConfig(config), gpioEventDescriptor(io),
        target(target), targets(targets), gpioLineMsg(lineMsg),
        continueAfterEvent(continueRun)
    {
        requestGPIOEvents();
    };

  private:
    /** @brief GPIO line */
    /* @brief GPIO line */
    gpiod_line* gpioLine;

    /** @brief GPIO line configuration */
    /* @brief GPIO line 配置 */
    gpiod_line_request_config gpioConfig;

    /** @brief GPIO event descriptor */
    /* @brief GPIO 事件描述符 */
    boost::asio::posix::stream_descriptor gpioEventDescriptor;

    /** @brief Systemd unit to be started when the condition is met */
    /* @brief 条件满足时要启动的 systemd unit */
    const std::string target;

    /** @brief Multi systemd units to be started when the condition is met */
    /* @brief 条件满足时要启动的多个 systemd unit */
    std::map<std::string, std::vector<std::string>> targets;

    /** @brief GPIO line name message */
    /* @brief GPIO line 名称消息 */
    std::string gpioLineMsg;

    /** @brief If the monitor should continue after event */
    /* @brief 事件发生后监控器是否应继续 */
    bool continueAfterEvent;

    /** @brief register handler for gpio event
     *
     *  @return  - 0 on success and -1 otherwise
     */
    /* @brief 为 GPIO 事件注册处理函数
     *  @return  - 成功返回 0，否则返回 -1 */
    int requestGPIOEvents();

    /** @brief Schedule an event handler for GPIO event to trigger */
    /* @brief 调度一个事件处理函数以触发 GPIO 事件 */
    void scheduleEventHandler();

    /** @brief Handle the GPIO event and starts configured target */
    /* @brief 处理 GPIO 事件并启动配置的 target */
    void gpioEventHandler();

    /** @brief handle current gpio value */
    /* @brief 处理当前 GPIO 值 */
    void gpioHandleInitialState(bool value);
};

} // namespace gpio
} // namespace phosphor
