// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2016 IBM Corporation

#include "monitor.hpp"

#include <systemd/sd-event.h>

#include <CLI/CLI.hpp>
#include <phosphor-logging/lg2.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    CLI::App app{"Monitor GPIO line for requested state change"};

    // Read arguments.
    // 读取参数。
    std::string path{};
    std::string key{};
    std::string polarity{};
    std::string target{};
    bool continueRun = false;

    /* Add an input option */
    /* 添加一个输入选项 */
    app.add_option("-p,--path", path,
                   "Path of input device. Ex: /dev/input/event2")
        ->required();
    app.add_option("-k,--key", key, "Input GPIO key number")->required();
    app.add_option("-r,--polarity", polarity,
                   "Assertion polarity to look for. This is 0 / 1")
        ->required();
    app.add_option("-t,--target", target,
                   "Systemd unit to be called on GPIO state change")
        ->required();
    app.add_flag("-c,--continue", continueRun,
                 "Whether or not to continue after key pressed");

    /* Due to the way this process is loaded from systemd, we can end up with
     * an empty extra parameter. Go ahead and ignore it. */
    /* 由于该进程由 systemd 加载的方式，最终可能会
     * 出现一个空的额外参数，直接忽略它即可。 */
    app.allow_extras();

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

    sd_event* event = nullptr;
    auto r = sd_event_default(&event);
    if (r < 0)
    {
        lg2::error("Error creating a default sd_event handler");
        return r;
    }
    phosphor::gpio::EventPtr eventP{event};
    event = nullptr;

    // Create a monitor object and let it do all the rest
    // 创建一个监控对象，其余工作交由它完成
    phosphor::gpio::Monitor monitor(path, std::stoi(key), std::stoi(polarity),
                                    target, eventP, continueRun);

    // Wait for client requests until this application has processed
    // at least one expected GPIO state change
    // 等待客户端请求，直到本应用已处理
    // 至少一个预期的 GPIO 状态变化
    while (!monitor.completed())
    {
        // -1 denotes wait for ever
        // -1 表示永久等待
        r = sd_event_run(eventP.get(), (uint64_t)-1);
        if (r < 0)
        {
            lg2::error("Failure in processing request: {RC}", "RC", r);
            return r;
        }
    }

    return 0;
}
