// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#include "evdev.hpp"
#include "monitor.hpp"

#include <linux/input.h>
#include <sys/types.h>

#include <chrono>
#include <iostream>
#include <string>

#include <gtest/gtest.h>

using namespace phosphor::gpio;

// Exit helper. Ideally should be class but need
// this to be used inside a static method.
// 退出辅助变量。理想情况下应为类，但需要
// 在静态方法中使用它。
bool completed{};

class GpioTest : public ::testing::Test
{
  public:
    static constexpr auto DEVICE = "/tmp/test_fifo";

    // systemd event handler
    // systemd 事件句柄
    sd_event* events;

    // Really needed just for the constructor
    // 仅构造函数需要
    decltype(input_event::code) code = 10;

    // Really needed just for the constructor
    // 仅构造函数需要
    decltype(input_event::value) value = 10;

    // Need this so that events can be initialized.
    // 需要它以便初始化事件。
    int rc;

    // Gets called as part of each TEST_F construction
    // 在每次 TEST_F 构造时调用
    GpioTest() : rc(sd_event_default(&events))
    {
        // Check for successful creation of event handler
        // 检查事件句柄是否创建成功
        EXPECT_GE(rc, 0);

        // FIFO created to simulate data available
        // 创建 FIFO 以模拟有数据可用
        EXPECT_EQ(0, mknod(DEVICE, S_IFIFO | 0666, 0));
    }

    // Gets called as part of each TEST_F destruction
    // 在每次 TEST_F 析构时调用
    ~GpioTest()
    {
        EXPECT_EQ(0, remove(DEVICE));

        events = sd_event_unref(events);
        EXPECT_EQ(events, nullptr);
    }

    // Callback handler on data
    // 有数据时的回调处理函数
    static int callbackHandler(sd_event_source*, int, uint32_t, void*)
    {
        std::cout << "Event fired" << std::endl;
        completed = true;
        return 0;
    }
};

/** @brief Makes sure that event never comes for 3 seconds
 */
/* @brief 确保 3 秒内不会到来任何事件 */
TEST_F(GpioTest, noEventIn3Seconds)
{
    using namespace std::chrono;

    phosphor::gpio::EventPtr eventP{events};
    events = nullptr;

    const std::string emptyTarget = "";
    Monitor gpio(DEVICE, code, value, emptyTarget, eventP, false,
                 callbackHandler, false);

    // Waiting 3 seconds and check if the completion status is set
    // 等待 3 秒并检查完成状态是否被置位
    int count = 0;
    while (count < 3)
    {
        // Returns -0- on timeout and positive number on dispatch
        // 超时返回 0，分发事件时返回正数
        auto sleepTime = duration_cast<microseconds>(seconds(1));
        if (!sd_event_run(eventP.get(), sleepTime.count()))
        {
            count++;
        }
    }
    EXPECT_EQ(false, completed);

    // 3 to cater to another uptick that happens prior to breaking.
    // 用 3 是为了计入退出前发生的另一次计数递增。
    EXPECT_EQ(3, count);
}

/** @brief Pump data in the middle and expect the callback to be invoked */
/* @brief 中途注入数据并期望回调被调用 */
TEST_F(GpioTest, pumpDataAndExpectCallBack)
{
    using namespace std::chrono;

    phosphor::gpio::EventPtr eventP{events};
    events = nullptr;

    const std::string emptyTarget = "";
    Monitor gpio(DEVICE, code, value, emptyTarget, eventP, false,
                 callbackHandler, false);

    // Pump the data in the middle
    // 在中途注入数据
    int count = 0;
    while (count < 2 && !completed)
    {
        if (count == 1)
        {
            auto pumpData = std::string("echo 'foo' > ") + DEVICE;
            EXPECT_GE(0, system(pumpData.c_str()));
        }

        // Returns -0- on timeout
        // 超时返回 0
        auto sleepTime = duration_cast<microseconds>(seconds(1));
        if (!sd_event_run(eventP.get(), sleepTime.count()))
        {
            count++;
        }
    }
    EXPECT_EQ(true, completed);
    EXPECT_EQ(1, count);
}
