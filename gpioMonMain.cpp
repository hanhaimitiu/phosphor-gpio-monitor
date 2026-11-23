// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#include "gpioMon.hpp"

#include <CLI/CLI.hpp>
#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>

#include <fstream>

namespace phosphor
{
namespace gpio
{

std::map<std::string, int> polarityMap = {
    /**< Only watch falling edge events. */
    /* < 仅监控下降沿事件。 */
    {"FALLING", GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE},
    /**< Only watch rising edge events. */
    /* < 仅监控上升沿事件。 */
    {"RISING", GPIOD_LINE_REQUEST_EVENT_RISING_EDGE},
    /**< Monitor both types of events. */
    /* < 同时监控两种类型的事件。 */
    {"BOTH", GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES}};

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
} // namespace gpio
} // namespace phosphor

int main(int argc, char** argv)
{
    boost::asio::io_context io;

    CLI::App app{"Monitor GPIO line for requested state change"};

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
        lg2::error("GPIO monitor config file not found: {FILE}", "FILE",
                   gpioFileName);
        return -1;
    }

    nlohmann::json gpioMonObj;
    file >> gpioMonObj;
    file.close();

    std::vector<std::unique_ptr<phosphor::gpio::GpioMonitor>> gpios;

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

        /* flag to monitor */
        /* 监控标志 */
        bool flag = false;

        /* target to start */
        /* 要启动的 target */
        std::string target;

        /* multi targets to start */
        /* 要启动的多个 target */
        std::map<std::string, std::vector<std::string>> targets;

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

            std::string chipIdStr = obj["ChipId"];
            int gpioNum = obj["GpioNum"];

            lineMsg += std::to_string(gpioNum);

            /* Get the GPIO line */
            /* 获取 GPIO line */
            line = gpiod_line_get(chipIdStr.c_str(), gpioNum);
        }
        else
        {
            /* Find the GPIO line */
            /* 查找 GPIO line */
            std::string lineName = obj["LineName"];
            lineMsg += lineName;
            line = gpiod_line_find(lineName.c_str());
        }

        if (line == nullptr)
        {
            lg2::error("Failed to find the {GPIO}", "GPIO", lineMsg);
            continue;
        }

        /* Get event to be monitored, if it is not defined then
         * Both rising falling edge will be monitored.
         */
        /* 获取要监控的事件，如果未定义，
         * 则同时监控上升沿和下降沿。 */
        if (obj.find("EventMon") != obj.end())
        {
            std::string eventStr = obj["EventMon"];
            auto findEvent = phosphor::gpio::polarityMap.find(eventStr);
            if (findEvent == phosphor::gpio::polarityMap.end())
            {
                lg2::error("{GPIO}: event missing: {EVENT}", "GPIO", lineMsg,
                           "EVENT", eventStr);
                return -1;
            }

            config.request_type = findEvent->second;
        }

        /* Get flag if monitoring needs to continue after first event */
        /* 获取标志：第一个事件之后是否仍需继续监控 */
        if (obj.find("Continue") != obj.end())
        {
            flag = obj["Continue"];
        }

        /* Parse out target argument. It is fine if the user does not
         * pass this if they are not interested in calling into any target
         * on meeting a condition.
         */
        /* 解析 target 参数。如果用户不关心条件满足时
         * 调用任何 target，则不传此参数
         * 也是可以的。 */
        if (obj.find("Target") != obj.end())
        {
            target = obj["Target"];
        }

        /* Parse out the targets argument if multi-targets are needed.*/
        /* 如果需要多个 target，则解析 targets 参数。 */
        if (obj.find("Targets") != obj.end())
        {
            obj.at("Targets").get_to(targets);
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

        /* Create a monitor object and let it do all the rest */
        /* 创建一个监控对象，其余工作交由它完成 */
        gpios.push_back(std::make_unique<phosphor::gpio::GpioMonitor>(
            line, config, io, target, targets, lineMsg, flag));
    }
    io.run();

    return 0;
}
