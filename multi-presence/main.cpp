// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#include "gpio_presence.hpp"

#include <CLI/CLI.hpp>
#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>

#include <fstream>

namespace phosphor
{
namespace gpio
{

const std::map<std::string, int> biasMap = {
    /**< Set bias as is. */
    /* < 保持偏置设置不变。 */
    {"AS_IS", 0},
    /**< Disable bias. */
    /* < 禁用偏置。 */
    {"DISABLE", GPIOD_LINE_REQUEST_FLAG_BIAS_DISABLE},
    /**< Enable pull-up. */
    /* < 启用上拉。 */
    {"PULL_UP", GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP},
    /**< Enable pull-down. */
    /* < 启用下拉。 */
    {"PULL_DOWN", GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN}};
}
} // namespace phosphor

int main(int argc, char** argv)
{
    boost::asio::io_context io;

    CLI::App app{"Monitor gpio presence status"};

    std::string gpioFileName;

    /* Add an input option */
    /* 添加一个输入选项 */
    app.add_option("-c,--config", gpioFileName, "Name of config json file")
        ->required()
        ->check(CLI::ExistingFile);

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

    /* Get list of gpio config details from json file */
    /* 从 json 文件中获取 GPIO 配置详情列表 */
    std::ifstream file(gpioFileName);
    if (!file)
    {
        lg2::error("Failed to open config file: {FILE}", "FILE", gpioFileName);
        return -1;
    }

    nlohmann::json gpioMonObj;
    file >> gpioMonObj;
    file.close();

    std::vector<phosphor::gpio::GpioPresence> gpios;

    for (auto& obj : gpioMonObj)
    {
        /* GPIO Line message */
        /* GPIO line 消息 */
        std::string lineMsg = "GPIO Line ";

        /* GPIO line */
        /* GPIO line */
        gpiod_line* line = nullptr;

        /* GPIO line configuration, default to monitor both edge */
        /* GPIO line 配置，默认监控双边沿 */
        struct gpiod_line_request_config config{
            "gpio_monitor", GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES, 0};

        /* Pretty name of the inventory object */
        /* inventory 对象的友好名称 */
        std::string name;

        /* Object path under inventory that will be created */
        /* 将要创建的 inventory 下的对象路径 */
        std::string inventory;

        /* List of interfaces to associate to inventory item */
        /* 要关联到 inventory 条目的接口列表 */
        std::vector<std::string> extraInterfaces;

        if (obj.find("LineName") == obj.end())
        {
            /* If there is no line Name defined then gpio num and chip
             * id must be defined. GpioNum is integer mapping to the
             * GPIO key configured by the kernel
             */
            /* 如果没有定义 line 名称，则必须定义 gpio num 和 chip
             * id。GpioNum 是映射到内核配置的
             * GPIO key 的整数 */
            if (obj.find("GpioNum") == obj.end() ||
                obj.find("ChipId") == obj.end())
            {
                lg2::error("Failed to find line name or gpio number: {FILE}",
                           "FILE", gpioFileName);
                return -1;
            }

            std::string chipIdStr = obj["ChipId"].get<std::string>();
            int gpioNum = obj["GpioNum"].get<int>();

            lineMsg += chipIdStr + " " + std::to_string(gpioNum);

            /* Get the GPIO line */
            /* 获取 GPIO line */
            line = gpiod_line_get(chipIdStr.c_str(), gpioNum);
        }
        else
        {
            /* Find the GPIO line */
            /* 查找 GPIO line */
            std::string lineName = obj["LineName"].get<std::string>();
            lineMsg += lineName;
            line = gpiod_line_find(lineName.c_str());
        }

        if (line == nullptr)
        {
            lg2::error("Failed to find the {GPIO}", "GPIO", lineMsg);
            continue;
        }

        /* Parse out inventory argument. */
        /* 解析 inventory 参数。 */
        if (obj.find("Inventory") == obj.end())
        {
            lg2::error("{GPIO}: Inventory path not specified", "GPIO", lineMsg);
            return -1;
        }
        else
        {
            inventory = obj["Inventory"].get<std::string>();
        }

        if (obj.find("Name") == obj.end())
        {
            lg2::error("{GPIO}: Name path not specified", "GPIO", lineMsg);
            return -1;
        }
        else
        {
            name = obj["Name"].get<std::string>();
        }

        /* Parse optional bias */
        /* 解析可选的偏置设置 */
        if (obj.find("Bias") != obj.end())
        {
            std::string biasName = obj["Bias"].get<std::string>();
            auto findBias = phosphor::gpio::biasMap.find(biasName);
            if (findBias == phosphor::gpio::biasMap.end())
            {
                lg2::error("{GPIO}: Bias unknown: {BIAS}", "GPIO", lineMsg,
                           "BIAS", biasName);
                return -1;
            }

            config.flags = findBias->second;
        }

        /* Parse optional active level */
        /* 解析可选的有效电平 */
        if (obj.find("ActiveLow") != obj.end() && obj["ActiveLow"].get<bool>())
        {
            config.flags |= GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW;
        }

        /* Parse optional extra interfaces */
        /* 解析可选的额外接口 */
        if (obj.find("ExtraInterfaces") != obj.end())
        {
            obj.at("ExtraInterfaces").get_to(extraInterfaces);
        }

        /* Create a monitor object and let it do all the rest */
        /* 创建一个监控对象，其余工作交由它完成 */
        gpios.push_back(phosphor::gpio::GpioPresence(
            line, config, io, inventory, extraInterfaces, name, lineMsg));
    }
    io.run();

    return 0;
}
