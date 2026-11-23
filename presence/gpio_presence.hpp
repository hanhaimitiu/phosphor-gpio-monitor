// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#pragma once
#include "evdev.hpp"

#include <systemd/sd-event.h>

#include <sdbusplus/bus.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace phosphor
{
namespace gpio
{
namespace presence
{

static constexpr auto deviceField = 0;
static constexpr auto pathField = 1;
using Device = std::string;
using Path = std::filesystem::path;
using Driver = std::tuple<Device, Path>;
using Interface = std::string;

/** @class Presence
 *  @brief Responsible for determining and monitoring presence,
 *  by monitoring GPIO state changes, of inventory items and
 *  updating D-Bus accordingly.
 */
/* @class Presence
 *  @brief 负责通过监控 GPIO 状态变化来确定和监控
 *  inventory 条目的在位状态，并相应地更新
 *  D-Bus。 */
class Presence : public Evdev
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
    Presence() = delete;
    ~Presence() = default;
    Presence(const Presence&) = delete;
    Presence& operator=(const Presence&) = delete;
    Presence(Presence&&) = delete;
    Presence& operator=(Presence&&) = delete;

    /** @brief Constructs Presence object.
     *
     *  @param[in] bus       - D-Bus bus Object
     *  @param[in] inventory - Object path under inventory
                               to display this inventory item
     *  @param[in] path      - Device path to read for GPIO pin state
                               to determine presence of inventory item
     *  @param[in] key       - GPIO key to monitor
     *  @param[in] name      - Pretty name of the inventory item
     *  @param[in] event     - sd_event handler
     *  @param[in] drivers   - list of device drivers to bind and unbind
     *  @param[in] ifaces    - list of extra interfaces to associate with the
     *                         inventory item
     *  @param[in] handler   - IO callback handler. Defaults to one in this
     *                        class
     */
    /* @brief 构造 Presence 对象。
     *  @param[in] bus       - D-Bus bus 对象
     *  @param[in] inventory - 用于显示该 inventory 条目的
     * Inventory 下的对象路径
     *  @param[in] path      - 用于读取 GPIO 引脚状态以确定
     * inventory 条目在位状态的设备路径
     *  @param[in] key       - 要监控的 GPIO key
     *  @param[in] name      - inventory 条目的友好名称
     *  @param[in] event     - sd_event 句柄
     *  @param[in] drivers   - 要绑定和解绑的设备驱动列表
     *  @param[in] ifaces    - 要关联到 inventory 条目的
     *                         额外接口列表
     *  @param[in] handler   - IO 回调处理函数，默认为本
     *                        类中的实现 */
    Presence(sdbusplus::bus_t& bus, const std::string& inventory,
             const std::string& path, const unsigned int key,
             const std::string& name, EventPtr& event,
             const std::vector<Driver>& drivers,
             const std::vector<Interface>& ifaces,
             sd_event_io_handler_t handler = Presence::processEvents) :
        Evdev(path, key, event, handler, true), bus(bus), inventory(inventory),
        name(name), drivers(drivers), ifaces(ifaces)
    {
        // See if the environment (from configuration file?) has a
        // DRIVER_BIND_DELAY_MS set.
        // 检查环境（来自配置文件？）中是否设置了
        // DRIVER_BIND_DELAY_MS。
        if (char* envDelay = std::getenv("DRIVER_BIND_DELAY_MS"))
        {
            // DRIVER_BIND_DELAY_MS environment variable is set.
            // Update the bind delay (in milliseconds) to the value from the
            // environment.
            // DRIVER_BIND_DELAY_MS 环境变量已设置。
            // 将绑定延迟（以毫秒为单位）更新为来自
            // 环境的值。
            delay = std::strtoull(envDelay, nullptr, 10);
        }
        determinePresence();
    }

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

  private:
    /**
     * @brief Update the present property for the inventory item.
     *
     * @param[in] present - What the present property should be set to.
     */
    /* @brief 更新 inventory 条目的 present 属性。
     * @param[in] present - present 属性应被设置的值。 */
    void updateInventory(bool present);

    /**
     * @brief Construct the inventory object map for the inventory item.
     *
     * @param[in] present - What the present property should be set to.
     *
     * @return The inventory object map to update inventory
     */
    /* @brief 为 inventory 条目构造 inventory 对象映射。
     * @param[in] present - present 属性应被设置的值。
     * @return 用于更新 inventory 的 inventory 对象映射 */
    ObjectMap getObjectMap(bool present);

    /** @brief Connection for sdbusplus bus */
    /* @brief sdbusplus bus 的连接 */
    sdbusplus::bus_t& bus;

    /**
     * @brief Read the GPIO device to determine initial presence and set
     *        present property at D-Bus path.
     */
    /* @brief 读取 GPIO 设备以确定初始在位状态，并在
     *        D-Bus 路径上设置 present 属性。 */
    void determinePresence();

    /** @brief Object path under inventory to display this inventory item */
    /* @brief 用于显示该 inventory 条目的 inventory 下的对象路径 */
    const std::string inventory;

    /** @brief Delay in milliseconds from present to bind device driver */
    /* @brief 从在位到绑定设备驱动的延迟（毫秒） */
    unsigned int delay = 0;

    /** @brief Pretty name of the inventory item*/
    /* @brief inventory 条目的友好名称 */
    const std::string name;

    /** @brief Analyzes the GPIO event and update present property*/
    /* @brief 分析 GPIO 事件并更新 present 属性 */
    void analyzeEvent();

    /** @brief  Vector of path and device tuples to bind/unbind*/
    /* @brief  要绑定/解绑的 path 与 device 元组的向量 */
    const std::vector<Driver> drivers;

    /** @brief  Vector of extra inventory interfaces to associate with the
     *          inventory item
     */
    /* @brief  要关联到 inventory 条目的额外 inventory
     *          接口向量 */
    const std::vector<Interface> ifaces;

    /**
     * @brief Binds or unbinds drivers
     *
     * Called when a presence change is detected to either
     * bind the drivers for the new card or unbind them for
     * the just removed card.  Operates on the drivers vector.
     *
     * Writes <device> to <path>/bind (or unbind)
     *
     * @param present - when true, will bind the drivers
     *                  when false, will unbind them
     */
    /* @brief 绑定或解绑驱动
     * 当检测到在位状态变化时调用，用于为新插入的
     * 板卡绑定驱动，或为刚拔出的板卡解绑驱动。
     * 操作对象是 drivers 向量。
     * 将 <device> 写入 <path>/bind（或 unbind）
     * @param present - 为 true 时绑定驱动
     *                  为 false 时解绑驱动 */
    void bindOrUnbindDrivers(bool present);
};

/**
 * @brief Get the service name from the mapper for the
 *        interface and path passed in.
 *
 * @param[in] path      - The D-Bus path name
 * @param[in] interface - The D-Bus interface name
 * @param[in] bus       - The D-Bus bus object
 *
 * @return The service name
 */
/* @brief 从 mapper 获取所传入的接口和路径对应的
 *        服务名。
 * @param[in] path      - D-Bus 路径名
 * @param[in] interface - D-Bus 接口名
 * @param[in] bus       - D-Bus bus 对象
 * @return 服务名 */
std::string getService(const std::string& path, const std::string& interface,
                       sdbusplus::bus_t& bus);

} // namespace presence
} // namespace gpio
} // namespace phosphor
