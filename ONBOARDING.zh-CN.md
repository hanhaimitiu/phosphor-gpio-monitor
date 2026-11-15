# phosphor-gpio-monitor 代码导读（OpenBMC 新人向）

> 面向刚转入 OpenBMC / openubmc 开发的同学。
> 建议先了解：libgpiod（v2 API）基本用法、libevdev、boost::asio 事件循环、
> OpenBMC 的 systemd target 集成方式。

## 1. 这个仓库是干什么的

监控 GPIO 电平变化，然后做出反应。仓库里其实是 **4 个独立守护进程**（都在 meson 里各自编译）：

| 可执行程序 | 目录 | 作用 | 底层技术 |
|---|---|---|---|
| `phosphor-gpio-monitor` | 根目录 `gpioMon.*` + `monitor.*` | 命令行指定单个 GPIO，事件发生时启动 systemd target（如 `obmc-chassis-hard-poweroff@0.target`） | libgpiod + boost::asio |
| `phosphor-multi-gpio-monitor` | 根目录 `mainapp.cpp` | 读 JSON 配置文件，单进程监控多条 GPIO 线，按事件启动 target | libgpiod + boost::asio |
| `phosphor-gpio-presence` | `presence/` | 通过 evdev 键盘事件（GPIO key 驱动）判断设备在位，更新 inventory D-Bus 的 `Present` 属性，并可 bind/unbind 内核驱动 | libevdev + sd-event |
| `phosphor-multi-gpio-presence` | `multi-presence/` | 新版在位检测：直接用 libgpiod 监控 GPIO，逻辑同上 | libgpiod + boost::asio |

一句话：**monitor 管"GPIO 变了要不要拉 systemd"，presence 管"设备插没插、上没上报 inventory"。**

## 2. 目录结构

```
├── gpioMon.hpp/.cpp        # GpioMonitor 类：单线监控 → 启动 target
├── gpioMonMain.cpp         # phosphor-gpio-monitor 的 main（解析命令行参数）
├── monitor.hpp/.cpp        # Monitor：事件处理 + systemd 调用（completed()）
├── mainapp.cpp             # phosphor-multi-gpio-monitor 的 main（读 JSON 配置）
├── evdev.hpp/.cpp          # Evdev 基类：封装 evdev 设备打开/事件读取
├── file.hpp                # File RAII 封装（open/close）
├── presence/               # phosphor-gpio-presence（evdev 版）
│   ├── main.cpp            #   main + getDrivers（读环境变量拼驱动列表）
│   └── gpio_presence.hpp/.cpp  # Presence 类：determinePresence / analyzeEvent /
│                             #   bindOrUnbindDrivers / updateInventory
├── multi-presence/         # phosphor-multi-gpio-presence（libgpiod 版）
│   ├── main.cpp            #   main：解析 JSON 配置、逐条建 GpioPresence
│   └── gpio_presence.hpp/.cpp  # GpioPresence 类：requestGPIOEvents /
│                             #   gpioEventHandler / scheduleEventHandler
└── test/utest.cpp          # gtest 单测
```

## 3. 核心流程

### 3.1 monitor / multi-gpio-monitor（libgpiod → systemd）

```
mainapp.cpp: main()
  读 JSON 配置（Name/LineName/EventMon/Target/Targets/Continue...）
  对每条线构造 GpioMonitor
      └─ requestGPIOEvents()            # gpiod_line_request + 拿 event fd
          └─ scheduleEventHandler()     # boost::asio::posix::stream_descriptor
                                        #   async_wait(read) 异步等 GPIO 事件
              └─ gpioEventHandler()     # 读 gpiod_line_event
                  ├─ gpioHandleInitialState()  # 启动时按当前电平触发 INIT_HIGH/INIT_LOW
                  ├─ 启动 target / targets（调用 systemd StartUnit）
                  └─ Continue=false 则退出；true 则再次 scheduleEventHandler
```

关键点：
- **异步模型**：GPIO 事件的 fd 交给 boost::asio 的 `posix::stream_descriptor`，
  `async_wait` 回调驱动一切 —— 这是 OpenBMC 新式守护进程的通用写法。
- **启动初始状态**：进程起来时会先读一次电平，按 `Targets` 里的 `INIT_HIGH` /
  `INIT_LOW` 键决定是否先启动某些 target（处理"上电前就处于某状态"的场景）。
- `monitor.cpp` 的 `Monitor` 类是旧路径（gpioMon 用的动作执行器），`completed()`
  负责真正去 `StartUnit`。

### 3.2 presence（evdev 版，presence/）

```
Presence 构造 → determinePresence()     # libevdev_fetch_event_value 读当前按键值
            → analyzeEvent()（FD 回调 processEvents）
                ├─ 上升沿 → present=true：可先 sleep DRIVER_BIND_DELAY_MS 再 bind 驱动
                ├─ updateInventory(present)   # mapper 查 inventory 服务 → Notify 更新 Present
                └─ bindOrUnbindDrivers()      # 往 /sys 写 driver bind/unbind
```

- 驱动 bind/unbind 列表来自命令行/环境变量（`getDrivers()`、`DRIVER_BIND_DELAY_MS`）。
- 在位状态写到 inventory 的 `xyz.openbmc_project.Inventory.Item` 的 `Present` 属性。

### 3.3 multi-presence（libgpiod 版，multi-presence/）

与 presence 同构，但事件来源是 libgpiod 而非 evdev：
`requestGPIOEvents()`（`gpiod_line_request` + `gpiod_line_event_get_fd` → asio descriptor）
→ `gpioEventHandler()` → 按 `GPIOD_LINE_EVENT_RISING_EDGE` 判断在位 → `updateInventory()`。
日志统一用 `lg2`（phosphor-logging 的结构化日志宏）。

### 3.4 两条"老路径"的实现细节（读老机器代码会遇到）

- **monitor/（evdev 单线版）**：`Monitor : public Evdev`（monitor.hpp:25），
  用 sd-event 的 IO 回调 `processEvents` 读 evdev 事件，`analyzeEvent()` 按配置的
  `polarity`（键值）匹配后启动 target；`completed()` 表示本条监控已达成。
  它比 multi 版早，用 `/dev/input` 设备而不是 libgpiod。
- **presence/ 的驱动 bind**：`Presence` 构造函数读环境变量
  `DRIVER_BIND_DELAY_MS`（presence/gpio_presence.hpp:82）控制在位后再 bind 驱动
  的延迟——这是给"设备上电后内核需要等一会才能 bind"的硬件 workaround。

### 3.5 systemd 调用与初始电平（gpioMon.cpp）

- 所有"启动 target"最终都是对 `org.freedesktop.systemd1` 的
  `Manager.StartUnit`（gpioMon.cpp:15 的常量 + 调用处）；
- `gpioHandleInitialState()`（gpioMon.cpp:112）：进程启动先读当前电平，
  若配置了 `Targets` 里的 `INIT_HIGH`/`INIT_LOW` 就先启动对应 target——
  这解决了"BMC 重启时设备本来就处于某状态"的恢复问题；
- `Continue` 为 false 时事件处理完进程退出（由 systemd unit 的 Restart 策略配合），
  true 时 `scheduleEventHandler()` 重新挂异步等待，循环监控。

## 4. 两个 D-Bus 习惯用法（新人必看）

- **不写死服务名**：`getService()` 用 mapper（`GetObject`）反查 inventory 的 bus name，
  这是 OpenBMC 全仓库的惯例，因为服务名可被机器层重定向。
- **更新 inventory**：拼 `ObjectMap`（path → interface → property → value），
  调 `xyz.openbmc_project.Inventory.Manager` 的 `Notify` 方法。

## 5. 配置

- multi-gpio-monitor 的 JSON 字段（README 有完整说明）：
  `Name`、`LineName`（推荐，按设备树里的线名找）、`GpioNum`/`ChipId`（备选）、
  `EventMon`（FALLING/RISING/BOTH）、`Target`、`Targets`（含 `INIT_HIGH`/`INIT_LOW`）、
  `Continue`（true=持续监控）。
- 机器层通过 bitbake bbappend 替换 `phosphor-multi-gpio-monitor.json` 完成平台适配。

## 6. 构建与测试

meson 构建，gtest 单测在 `test/utest.cpp`。整机镜像中由 bitbake 打包，
systemd unit 模板（如 `phosphor-gpio-monitor@.service`）也在仓库/元数据层提供。

## 7. 新手学习路径

1. `README.md` → 4 个守护进程的分工；
2. `multi-presence/main.cpp` → `GpioPresence` —— 最现代的一条代码路径（libgpiod + asio + lg2）；
3. `gpioMon.cpp` 的 `scheduleEventHandler` —— asio 异步等 fd 的模板写法；
4. `presence/gpio_presence.cpp` 的 `bindOrUnbindDrivers` —— 内核驱动 bind/unbind 实操；
5. 顺带理解 `evdev.hpp`（evdev 老路径，读现有机器代码时会遇到）。

## 8. 常见改动场景

- **平台适配**：改 JSON 配置（bbappend），不动代码。
- **新增 GPIO 动作**：在 `gpioEventHandler` / target 配置层扩展；优先走 systemd target 复用现有机制。
- **调试**：journal 里搜 GPIO 名；`gpiodetect`/`gpioinfo`/`gpiofind` 确认线名与事件；
  D-Bus 上用 `busctl` 检查 inventory 的 `Present` 是否更新。
