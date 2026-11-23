// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#pragma once
#include "file.hpp"

#include <libevdev/libevdev.h>
#include <systemd/sd-event.h>

#include <sdbusplus/message.hpp>

#include <map>
#include <memory>
#include <string>

namespace phosphor
{
namespace gpio
{

/* Need a custom deleter for freeing up sd_event */
/* 需要一个自定义删除器来释放 sd_event */
struct EventDeleter
{
    void operator()(sd_event* event) const
    {
        sd_event_unref(event);
    }
};
using EventPtr = std::unique_ptr<sd_event, EventDeleter>;

/* Need a custom deleter for freeing up sd_event_source */
/* 需要一个自定义删除器来释放 sd_event_source */
struct EventSourceDeleter
{
    void operator()(sd_event_source* eventSource) const
    {
        sd_event_source_unref(eventSource);
    }
};
using EventSourcePtr = std::unique_ptr<sd_event_source, EventSourceDeleter>;

/* Need a custom deleter for freeing up evdev struct */
/* 需要一个自定义删除器来释放 evdev 结构体 */
struct FreeEvDev
{
    void operator()(struct libevdev* device) const
    {
        libevdev_free(device);
    }
};
using EvdevPtr = std::unique_ptr<struct libevdev, FreeEvDev>;

/** @class Evdev
 *  @brief Responsible for catching GPIO state changes conditions and taking
 *  actions
 */
/* @class Evdev
 *  @brief 负责捕获 GPIO 状态变化情况并执行
 *  相应操作 */
class Evdev
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
    Evdev() = delete;
    ~Evdev() = default;
    Evdev(const Evdev&) = delete;
    Evdev& operator=(const Evdev&) = delete;
    Evdev(Evdev&&) = delete;
    Evdev& operator=(Evdev&&) = delete;

    /** @brief Constructs Evdev object.
     *
     *  @param[in] path      - Device path to read for GPIO pin state
     *  @param[in] key       - GPIO key to monitor
     *  @param[in] event     - sd_event handler
     *  @param[in] handler   - IO callback handler.
     *  @param[in] useEvDev  - Whether to use EvDev to retrieve events
     */
    /* @brief 构造 Evdev 对象。
     *  @param[in] path      - 用于读取 GPIO 引脚状态的设备路径
     *  @param[in] key       - 要监控的 GPIO key
     *  @param[in] event     - sd_event 句柄
     *  @param[in] handler   - IO 回调处理函数
     *  @param[in] useEvDev  - 是否使用 EvDev 来获取事件 */
    Evdev(const std::string& path, const unsigned int key, EventPtr& event,
          sd_event_io_handler_t handler, bool useEvDev = true) :
        path(path), key(key), event(event), callbackHandler(handler),
        fd(openDevice())

    {
        if (useEvDev)
        {
            // If we are asked to use EvDev, do that initialization.
            // 如果要求使用 EvDev，则执行相应的初始化。
            initEvDev();
        }

        // Register callback handler when FD has some data
        // 当 FD 有数据时注册回调处理函数
        registerCallback();
    }

  protected:
    /** @brief Device path to read for GPIO pin state */
    /* @brief 用于读取 GPIO 引脚状态的设备路径 */
    const std::string path;

    /** @brief GPIO key to monitor */
    /* @brief 要监控的 GPIO key */
    const unsigned int key;

    /** @brief Event structure */
    /* @brief 事件结构体 */
    EvdevPtr devicePtr;

    /** @brief Monitor to sd_event */
    /* @brief sd_event 监控对象 */
    EventPtr& event;

    /** @brief Callback handler when the FD has some data */
    /* @brief FD 有数据时的回调处理函数 */
    sd_event_io_handler_t callbackHandler;

    /** @brief event source */
    /* @brief 事件源 */
    EventSourcePtr eventSource;

    /** @brief Opens the device and populates the descriptor */
    /* @brief 打开设备并填充文件描述符 */
    int openDevice();

    /** @brief attaches FD to events and sets up callback handler */
    /* @brief 将 FD 附加到事件并设置回调处理函数 */
    void registerCallback();

    /** @brief File descriptor manager */
    /* @brief 文件描述符管理器 */
    FileDescriptor fd;

    /** @brief Initializes evdev handle with the fd */
    /* @brief 用 fd 初始化 evdev 句柄 */
    void initEvDev();
};

} // namespace gpio
} // namespace phosphor
