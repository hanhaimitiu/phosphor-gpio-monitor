// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#include "gpio_presence.hpp"

#include <systemd/sd-event.h>

#include <CLI/CLI.hpp>
#include <phosphor-logging/lg2.hpp>

#include <iostream>

using namespace phosphor::gpio;
using namespace phosphor::gpio::presence;

/**
 * Pulls out the path,device pairs from the string
 * passed in
 *
 * @param[in] driverString - space separated path,device pairs
 * @param[out] drivers - vector of device,path tuples filled in
 *                       from driverString
 *
 * @return int - 0 if successful, < 0 else
 */
/* 从传入的字符串中提取 path,device 对，
 * 字符串由调用方传入
 * @param[in] driverString - 以空格分隔的 path,device 对
 * @param[out] drivers - 由 driverString 填充的 device,path
 *                       元组向量
 * @return int - 成功返回 0，否则返回 < 0 */
static int getDrivers(const std::string& driverString,
                      std::vector<Driver>& drivers)
{
    std::istringstream stream{driverString};

    while (true)
    {
        std::string entry;

        // Extract each path,device pair
        // 提取每一对 path,device
        stream >> entry;

        if (entry.empty())
        {
            break;
        }

        // Extract the path and device and save them
        // 提取 path 和 device 并保存
        auto pos = entry.rfind(',');
        if (pos != std::string::npos)
        {
            auto path = entry.substr(0, pos);
            auto device = entry.substr(pos + 1);

            drivers.emplace_back(std::move(device), std::move(path));
        }
        else
        {
            lg2::error("Invalid path,device combination: {ENTRY}", "ENTRY",
                       entry);
            return -1;
        }
    }

    return 0;
}

int main(int argc, char** argv)
{
    CLI::App app{"Monitor gpio presence status"};

    std::string path{};
    std::string key{};
    std::string name{};
    std::string inventory{};
    std::string drivers{};
    std::string ifaces{};

    /* Add an input option */
    /* 添加一个输入选项 */
    app.add_option(
           "-p,--path", path,
           " Path of device to read for GPIO pin state to determine presence of inventory item")
        ->required();
    app.add_option("-k,--key", key, "Input GPIO key number")->required();
    app.add_option("-n,--name", name, "Pretty name of the inventory item")
        ->required();
    app.add_option("-i,--inventory", inventory,
                   "Object path under inventory that will be created")
        ->required();
    app.add_option(
           "-d,--drivers", drivers,
           "List of drivers to bind when card is added and unbind when card is removed\n"
           "Format is a space separated list of path,device pairs.\n"
           "For example: /sys/bus/i2c/drivers/some-driver,3-0068")
        ->expected(0, 1);
    app.add_option(
           "-e,--extra-ifaces", ifaces,
           "List of interfaces to associate to inventory item\n"
           "Format is a comma separated list of interfaces.\n"
           "For example: /xyz/openbmc_project/.../1,/xyz/openbmc_project/.../2")
        ->expected(0, 1);

    /* Parse input parameter */
    /* 解析输入参数 */
    try
    {
        app.parse(argc, argv);
    }
    catch (const CLI::Error& e)
    {
        return app.exit(e);
    }

    std::vector<Driver> driverList;

    // Driver list is optional
    // 驱动列表是可选的
    if (!drivers.empty())
    {
        if (getDrivers(drivers, driverList) < 0)
        {
            lg2::error("Failed to parser drivers: {DRIVERS}", "DRIVERS",
                       drivers);
            return -1;
        }
    }

    std::vector<Interface> ifaceList;

    // Extra interfaces list is optional
    // 额外接口列表是可选的
    if (!ifaces.empty())
    {
        std::stringstream ss(ifaces);
        Interface iface;
        while (std::getline(ss, iface, ','))
        {
            ifaceList.push_back(iface);
        }
    }

    auto bus = sdbusplus::bus::new_default();
    auto rc = 0;
    sd_event* event = nullptr;
    rc = sd_event_default(&event);
    if (rc < 0)
    {
        lg2::error("Error creating a default sd_event handler");
        return rc;
    }
    EventPtr eventP{event};
    event = nullptr;

    Presence presence(bus, inventory, path, std::stoul(key), name, eventP,
                      driverList, ifaceList);

    while (true)
    {
        // -1 denotes wait forever
        // -1 表示永久等待
        rc = sd_event_run(eventP.get(), (uint64_t)-1);
        if (rc < 0)
        {
            lg2::error("Failure in processing request: {RC}", "RC", rc);
            break;
        }
    }
    return rc;
}
