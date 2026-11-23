// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#pragma once

#include <gpiod.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

static constexpr auto deviceField = 0;
static constexpr auto pathField = 1;
using Device = std::string;
using Path = std::filesystem::path;
using Driver = std::tuple<Device, Path>;
using Interface = std::string;

namespace phosphor
{
namespace gpio
{

/** @class GpioPresence
 *  @brief Responsible for catching GPIO state change
 *  condition and updating the inventory presence.
 */
/* @class GpioPresence
 *  @brief 负责捕获 GPIO 状态变化
 *  条件并更新 inventory 在位状态。 */
class GpioPresence
{
    using Property = std::string;
    using Value = std::variant<bool, std::string>;
    // Association between property and its value
    // 属性与其值之间的关联
    using PropertyMap = std::map<Property, Value>;
    using Interface = std::string;
    // Association between interface and the D-Bus property
    // 接口与 D-Bus 属性之间的关联
    using InterfaceMap = std::map<Interface, PropertyMap>;
    using Object = sdbusplus::object_path;
    // Association between object and the interface
    // 对象与接口之间的关联
    using ObjectMap = std::map<Object, InterfaceMap>;

  public:
    GpioPresence() = delete;
    ~GpioPresence() = default;
    GpioPresence(const GpioPresence&) = delete;
    GpioPresence& operator=(const GpioPresence&) = delete;

    /** @brief Constructs GpioPresence object.
     *
     *  @param[in] line             - GPIO line from libgpiod
     *  @param[in] config           - configuration of line with event
     *  @param[in] io               - io service
     *  @param[in] inventory        - Object path under inventory that
                                      will be created
     *  @param[in] extraInterfaces  - List of interfaces to associate to
                                      inventory item
     *  @param[in] name             - PrettyName of inventory object
     *  @param[in] lineMsg          - GPIO line message to be used for log
     */
    /* @brief 构造 GpioPresence 对象。
     *  @param[in] line             - 来自 libgpiod 的 GPIO line
     *  @param[in] config           - 带事件的 line 配置
     *  @param[in] io               - io 服务
     *  @param[in] inventory        - 将要创建的 inventory 下的
     * 对象路径
     *  @param[in] extraInterfaces  - 要关联到 inventory 条目的
     * 接口列表
     *  @param[in] name             - inventory 对象的 PrettyName
     *  @param[in] lineMsg          - 用于日志的 GPIO line 消息 */
    GpioPresence(gpiod_line* line, gpiod_line_request_config& config,
                 boost::asio::io_context& io, const std::string& inventory,
                 const std::vector<std::string>& extraInterfaces,
                 const std::string& name, const std::string& lineMsg) :
        gpioLine(line), gpioConfig(config), gpioEventDescriptor(io),
        inventory(inventory), interfaces(extraInterfaces), name(name),
        gpioLineMsg(lineMsg)
    {
        requestGPIOEvents();
    };

    GpioPresence(GpioPresence&& old) noexcept :
        gpioLine(old.gpioLine), gpioConfig(old.gpioConfig),
        gpioEventDescriptor(old.gpioEventDescriptor.get_executor()),
        inventory(std::move(old.inventory)),
        interfaces(std::move(old.interfaces)), name(std::move(old.name)),
        gpioLineMsg(std::move(old.gpioLineMsg))
    {
        old.cancelEventHandler();

        gpioEventDescriptor = std::move(old.gpioEventDescriptor);

        scheduleEventHandler();
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

    /** @brief Object path under inventory that will be created */
    /* @brief 将要创建的 inventory 下的对象路径 */
    const std::string inventory;

    /** @brief List of interfaces to associate to inventory item */
    /* @brief 要关联到 inventory 条目的接口列表 */
    const std::vector<std::string> interfaces;

    /** @brief PrettyName of inventory object */
    /* @brief inventory 对象的 PrettyName */
    const std::string name;

    /** @brief GPIO line name message */
    /* @brief GPIO line 名称消息 */
    const std::string gpioLineMsg;

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

    /** @brief Stop the event handler for GPIO events */
    /* @brief 停止 GPIO 事件的事件处理函数 */
    void cancelEventHandler();

    /** @brief Handle the GPIO event and starts configured target */
    /* @brief 处理 GPIO 事件并启动配置的 target */
    void gpioEventHandler();

    /** @brief Returns the object map for the inventory object */
    /* @brief 返回 inventory 对象的对象映射 */
    ObjectMap getObjectMap(bool present);

    /** @brief Updates the inventory */
    /* @brief 更新 inventory */
    void updateInventory(bool present);
};

} // namespace gpio
} // namespace phosphor
