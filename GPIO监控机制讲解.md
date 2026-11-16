# GPIO 监控机制讲解：4 个守护进程内部到底怎么转

> 面向 Java 背景、长期没碰 C++ / 系统编程的人。先看真实例子，再点机制。
> 本文**深入内部机制**（asio 事件循环、调用链、INIT 恢复、mapper 反查、JSON 驱动）。
> 业务背景看《GPIO监控业务详解.md》，代码结构看 `ONBOARDING.zh-CN.md`。
>
> 📌 路径说明：所有路径相对仓库根目录 `phosphor-gpio-monitor/`。
> 根目录文件：`mainapp.cpp`、`monitor.cpp/.hpp`、`evdev.cpp/.hpp`、`gpioMonMain.cpp`、`gpioMon.cpp/.hpp`、`file.hpp`；
> 子目录文件：`presence/main.cpp`、`presence/gpio_presence.cpp/.hpp`、`multi-presence/main.cpp`、`multi-presence/gpio_presence.cpp/.hpp`。
> 所有 `文件:行号` 都用 `sed -n` 逐段核对过。

---

## 0. 先讲一个真实例子：电源按钮被按下后，进程内部发生了什么

假设运维按了前面板 `POWER_BUTTON`，跑着的新版监控进程 `phosphor-multi-gpio-monitor` 内部是这样转的：

```
进程早就启动好了，主线程停在一句 io.run() 上（gpioMonMain.cpp:196）。
      │
      │  --- 硬件层：电平从高变低，内核的 gpio 驱动在 event fd 上产生可读事件 ---
      ▼
boost::asio 的 io_context 发现 fd 可读
 → 触发之前挂的 async_wait(read) 回调（gpioMon.cpp:26-36）
 → 回调里调 gpioEventHandler()（gpioMon.cpp:35）
      │
      ▼
gpioEventHandler() 用 gpiod_line_event_read_fd 读出这个事件（gpioMon.cpp:43-44）
 → 判断是 RISING 还是 FALLING，打日志 Asserted/Deasserted（gpioMon.cpp:50-57）
 → 查配置：有 Target 就 StartUnit（gpioMon.cpp:59-69）
 → 有 Targets 就按边沿方向取一组 unit 逐个 StartUnit（gpioMon.cpp:71-100）
 → Continue=false → return，进程结束（gpioMon.cpp:102-106）
   Continue=true  → 再次 scheduleEventHandler()，回到 io.run() 继续等（gpioMon.cpp:108-109）
```

**这段就是整个仓库最核心的机制**：一个 fd + 一个异步等待回调 + 一个事件处理函数 + 一次 D-Bus 调用。
后面所有内容都是把这个例子展开、并补齐另外 3 个进程的变体。

> Java 类比：把 `io.run()` 想成 Netty 的 `EventLoop.run()` / `Selector.select()` 循环；
> `async_wait(read)` 想成给 channel 注册 `OP_READ`  interest + 回调；
> `gpioEventHandler()` 就是你的 `channelRead()`。整个进程是**单线程事件驱动**，不是"每条线一个线程"。

---

## 1. 先看构建关系：4 个进程分别由哪些源文件编译

理解机制前先把"哪个进程跑哪份代码"钉死。以 `meson.build` 为准（`ONBOARDING` 的表格把两个 monitor 写反了）：

| 进程 | 源文件 | 构建定义 | 事件库 | 事件循环 |
|---|---|---|---|---|
| `phosphor-gpio-monitor` | `mainapp.cpp` + `monitor.cpp` + `evdev.cpp` | `meson.build:110-116` | libevdev | `sd_event_run()` 手动循环 |
| `phosphor-multi-gpio-monitor` | `gpioMonMain.cpp` + `gpioMon.cpp` | `meson.build:118-133` | libgpiod | `boost::asio::io_context.run()` |
| `phosphor-gpio-presence` | `presence/main.cpp` + `presence/gpio_presence.cpp` + `evdev.cpp` | `presence/meson.build:1-10`、`meson.build:92-101` | libevdev | `sd_event_run()` 手动循环 |
| `phosphor-multi-gpio-presence` | `multi-presence/main.cpp` + `multi-presence/gpio_presence.cpp` | `multi-presence/meson.build:1-17` | libgpiod | `boost::asio::io_context.run()` |

> ⚠️ `mainapp.cpp` 才是 `phosphor-gpio-monitor` 的 main（`meson.build:112`），
> `gpioMonMain.cpp` 是 `phosphor-multi-gpio-monitor` 的 main（`meson.build:120`）。
> `ONBOARDING.zh-CN.md:11-16` 表格写反了，别被带偏。

由此得出**两条技术栈**：

```
老栈（evdev / gpio-keys / sd-event）：
  /dev/input/event* → libevdev_next_event → sd_event_add_io 回调 → 手动 while 循环
  代表：phosphor-gpio-monitor、phosphor-gpio-presence

新栈（libgpiod / boost::asio）：
  设备树 line name → gpiod_line_event_get_fd → asio stream_descriptor.async_wait → io.run()
  代表：phosphor-multi-gpio-monitor、phosphor-multi-gpio-presence
```

下文第 2~6 章讲**新栈**（也是你应优先掌握的），第 7~8 章讲**老栈**与 presence 差异。

---

## 2. asio 事件循环：GPIO 的 fd 是怎么被"等"起来的

新栈的所有魔法都在 `GpioMonitor::requestGPIOEvents()`（构造时就被调用，`gpioMon.hpp:52-54`）和 `scheduleEventHandler()` 这一对函数里。

### 2.1 从"找到线"到"挂上异步等待"的完整调用链

```
GpioMonitor 构造（gpioMonMain.cpp:193-194 创建）
  │
  ▼
GpioMonitor::GpioMonitor(...)            gpioMon.hpp:45-54
  └─ requestGPIOEvents()                 gpioMon.hpp:53
       │
       ├─ ① gpiod_line_request(line, &config, 0)    gpioMon.cpp:131
       │     向内核"申请"这条线，按 config 监听边沿
       │
       ├─ ② int fd = gpiod_line_event_get_fd(line)  gpioMon.cpp:137
       │     拿到一个 Linux 文件描述符（事件来了它变可读）
       │
       ├─ ③ int value = gpiod_line_get_value(line)  gpioMon.cpp:144
       │     读一次当前电平 → gpioHandleInitialState(value != 0)  gpioMon.cpp:152
       │     （启动恢复，见第 5 章）
       │
       ├─ ④ gpioEventDescriptor.assign(fd)          gpioMon.cpp:158
       │     把这个裸 fd 交给 boost::asio 的 stream_descriptor 托管
       │
       └─ ⑤ scheduleEventHandler()                  gpioMon.cpp:161
              └─ gpioEventDescriptor.async_wait(wait_read, lambda)   gpioMon.cpp:26-28
                 挂上一个"等 fd 可读"的异步回调，立即返回

main 最后 io.run()                       gpioMonMain.cpp:196
  → -blocking 地跑事件循环，等回调被触发
```

### 2.2 `scheduleEventHandler()` 逐行拆解

`gpioMon.cpp:24-37`：

```cpp
void GpioMonitor::scheduleEventHandler()
{
    gpioEventDescriptor.async_wait(                         // :26 给 fd 挂异步等待
        boost::asio::posix::stream_descriptor::wait_read,   // :27 等"可读"事件
        [this](const boost::system::error_code& ec) {       // :28 回调 lambda
            if (ec) {                                        // :29
                lg2::error(...);                             // :31-32 出错打日志
                return;                                      // :33
            }
            gpioEventHandler();                              // :35 没出错就处理事件
        });
}
```

要点：

1. **`async_wait` 是异步的**：调用后立刻返回，不阻塞。回调要等 `io.run()` 转起来、fd 真的可读时才被触发。
2. **`wait_read`**：等的是"这个 fd 上有数据可读"——内核在 GPIO 边沿到来时会往这个 fd 写一条 `gpiod_line_event` 结构。
3. **回调里先查 `error_code`**：这是 asio 的标准套路。Java 里 NIO 回调给你的是 `SelectionKey`；这里给你的是 `error_code`，错了就记日志退出。
4. **处理完要"重新挂"**：`gpioEventHandler()` 结尾在 `Continue=true` 时再次调 `scheduleEventHandler()`（`gpioMon.cpp:108-109`）——**asio 的 `async_wait` 是一次性的**，处理完一次得重新挂，否则只监控一次事件。

> Java 类比：这就像你用 `CompletableFuture` / Netty `ChannelFuture` 注册一个 `whenComplete` 回调，回调处理完后再注册下一个——不是 `while(true)` 轮询，而是"事件来了才醒"。

### 2.3 为什么不用"每条线一个线程"

`gpioMonMain.cpp:74-195` 把 JSON 里每条 GPIO 都 `make_unique<GpioMonitor>` 塞进 vector，但**只有一个 `io_context`、一次 `io.run()`**（`gpioMonMain.cpp:40、196`）。所有线的 fd 都注册到**同一个 asio 事件循环**上。

Java 类比：这就是 **Reactor 单线程模型**（Netty 的 `NioEventLoopGroup(1)`）——用少量线程处理大量连接/事件源，比"一条线一个线程"省资源、没有锁竞争。`meson.build:37-41` 里还加了 `-DBOOST_ASIO_DISABLE_THREADS`，明确**禁用 asio 的多线程支持**，坐实了单线程模型。

---

## 3. 事件到 target 的完整调用链（multi-gpio-monitor）

把第 0 章例子的"处理事件"部分展开到函数级，这是本仓库最核心的一条链。

### 3.1 `gpioEventHandler()` 全流程（`gpioMon.cpp:39-110`）

```
                       fd 可读，回调触发
                              │
                              ▼
        gpiod_line_event_read_fd(fd, &event)     :43-44
        │ 读失败 → 打日志 return                 :45-48
        ▼
        判断 event_type                           :50-57
        ├─ RISING_EDGE → info("{GPIO} Asserted")  :52
        └─ 其它        → info("{GPIO} Deasserted"):56
        ▼
        target 非空？                             :60
        ├─ 是 → new_default() 建 D-Bus 连接        :62
        │       new_method_call(systemd1, ...)     :63-64
        │       append(target); append("replace")  :65-66
        │       bus.call_noreply(method)           :68   ← 发出去，不等回执
        ▼
        Targets map 里按边沿取一组 unit            :71-87
        ├─ RISING → targets["RISING"]             :72-79
        └─ FALLING → targets["FALLING"]           :80-87
        ▼
        targetsToStart 非空？                      :90
        └─ 是 → 逐个 StartUnit（同样的 D-Bus 调用） :92-99
        ▼
        continueAfterEvent？                       :103
        ├─ false → return（进程即将无回调可跑而退出）:104-106
        └─ true  → scheduleEventHandler() 重新挂    :108-109
```

### 3.2 systemd 调用的三要素（`gpioMon.cpp:15-17`）

所有"拉 target"最终都是对 systemd 的 D-Bus 调用：

| 要素 | 值 | 代码位置 |
|---|---|---|
| service | `org.freedesktop.systemd1` | `gpioMon.cpp:15` |
| object path | `/org/freedesktop/systemd1` | `gpioMon.cpp:16` |
| interface + method | `org.freedesktop.systemd1.Manager.StartUnit` | `gpioMon.cpp:17、63-64` |
| 参数 | `(unit 名, "replace")` | `gpioMon.cpp:65-66、97` |

`"replace"` 表示：若该 target 已在运行，替换重启，保证"最新事件优先"。
`call_noreply` 表示**发完不等结果**——硬件事件处理不能卡在等 systemd 回执上。

> Java 类比：这相当于调用一个远程服务的 `start(unit, mode)` 接口，`mode="replace"` 是幂等覆盖策略，`call_noreply` 就是异步发送消息（`rabbitTemplate.convertAndSend` 那种发完不管）。

### 3.3 `Continue` 标志决定进程生死

- 配置里 `Continue: true` → `flag=true` 传进 `GpioMonitor`（`gpioMonMain.cpp:150-154、193-194`）→ 每次事件后重新挂等待，进程常驻。
- 不配或 `false` → 处理完一次事件就 `return`（`gpioMon.cpp:102-106`），`io.run()` 没有 pending 的回调就返回，`main` 结束（`gpioMonMain.cpp:196-198`）。systemd unit 里 `Restart=always`（`phosphor-multi-gpio-monitor.service:5`）再把它拉起来。

> 这是 OpenBMC 常见的"**一次性任务 + systemd 负责重启**"模式：进程自己不写死循环，靠外部 Restart 策略复活。Java 类比：像 K8s 的 Job 设了 `restartPolicy: Always`——任务退出由编排层负责重启。

---

## 4. 启动流程：JSON 是怎么把进程"喂"起来的

新版两个进程（`multi-gpio-monitor` 和 `multi-gpio-presence`）的 main 结构几乎一样。以 `gpioMonMain.cpp:38-199` 为例：

```
main(argc, argv)
  │
  ├─ boost::asio::io_context io;                        :40
  ├─ CLI11 解析 -c/--config（要求文件存在）              :42-49
  ├─ 读文件到 nlohmann::json gpioMonObj                  :62-72
  │
  ├─ for (auto& obj : gpioMonObj)                        :76  ← 遍历每条线的配置
  │    ├─ 找 GPIO 线：
  │    │   ├─ 有 LineName → gpiod_line_find(name)        :121-125
  │    │   └─ 否则要 GpioNum+ChipId → gpiod_line_get()   :97-118
  │    │   （都拿不到就报错 continue）                   :127-131
  │    ├─ 默认 config = BOTH_EDGES                       :84-86
  │    ├─ 解析 EventMon → config.request_type            :136-148
  │    ├─ 解析 Continue / Target / Targets               :150-169
  │    ├─ 解析 Bias / ActiveLow → config.flags           :171-190
  │    └─ gpios.push_back(make_unique<GpioMonitor>(...)) :192-194
  │          ↑ 构造函数里就 requestGPIOEvents() 挂好了异步等待
  │
  └─ io.run()                                            :196  ← 事件循环启动，此后不返回直到没回调
```

### 4.1 字符串到枚举：两张映射表

`gpioMonMain.cpp:18-34` 定义了两张 `std::map<std::string, int>`：

- `polarityMap`：`FALLING/RISING/BOTH` → libgpiod 的边沿常量（`:18-24`），被 `gpioMonMain.cpp:139-147` 查表填入 `config.request_type`。
- `biasMap`：`AS_IS/DISABLE/PULL_UP/PULL_DOWN` → libgpiod 的 bias flag（`:26-34`），被 `gpioMonMain.cpp:174-183` 查表填入 `config.flags`。

> Java 类比：这就是把配置文件里的字符串常量转成内部枚举值的"反序列化映射"，相当于你写 `EventType.valueOf(str)` 或 Jackson 的 `@JsonCreator`——只是这里手写了个 map。查不到就报错返回（`:140-144、176-180`），相当于"配置非法，启动失败"。

### 4.2 找线的两种姿势：按名字 vs 按编号

- **按名字**（推荐）：`gpiod_line_find(lineName)`（`gpioMonMain.cpp:124`）。名字来自设备树，板子改线序也不会错。仓库自带配置用的就是 `LineName: "POWER_BUTTON"`（`phosphor-multi-gpio-monitor.json:4`）。
- **按编号**（备选）：必须同时给 `GpioNum` 和 `ChipId`，然后 `gpiod_line_get(chipIdStr, gpioNum)`（`gpioMonMain.cpp:111-117`）。缺一个就报错（`gpioMonMain.cpp:103-109`）。

### 4.3 两个 JSON 的字段 → 代码解析位置对照

| 配置文件 | 字段 | 解析位置 |
|---|---|---|
| `phosphor-multi-gpio-monitor.json` | `LineName` | `gpioMonMain.cpp:121-125` |
| | `GpioNum`/`ChipId` | `gpioMonMain.cpp:97-118` |
| | `EventMon` | `gpioMonMain.cpp:136-148` |
| | `Continue` | `gpioMonMain.cpp:150-154` |
| | `Target` | `gpioMonMain.cpp:160-163` |
| | `Targets` | `gpioMonMain.cpp:165-169` |
| | `Bias` / `ActiveLow` | `gpioMonMain.cpp:171-190` |
| `phosphor-multi-gpio-presence.json` | `LineName` | `multi-presence/main.cpp:112-116` |
| | `GpioNum`/`ChipId` | `multi-presence/main.cpp:88-109` |
| | `Inventory` | `multi-presence/main.cpp:124-133` |
| | `Name` | `multi-presence/main.cpp:135-143` |
| | `Bias` / `ActiveLow` | `multi-presence/main.cpp:145-164` |
| | `ExtraInterfaces` | `multi-presence/main.cpp:166-170` |

完整的字段业务含义见《GPIO监控业务详解.md》第 7 章，原始说明见 `README.md:31-56、105-115`。

---

## 5. INIT_HIGH / INIT_LOW：启动时的电平恢复机制

### 5.1 为什么需要它（先讲问题）

BMC 重启（升级固件、看门狗复位）时，**主机侧的物理状态没变**：机箱盖还开着、PSU 还插着。但进程是全新启动的，**这期间没有任何边沿事件**——如果只靠"等事件"，进程会永远不知道"设备现在其实是在位/不在位"。

解决办法：**启动时主动读一次当前电平**，把它当成一个"虚拟事件"先处理掉。

### 5.2 代码怎么做的

`gpioMon.cpp:144-153`（在 `requestGPIOEvents()` 里，注意发生在 `scheduleEventHandler()` 之前）：

```cpp
int value = gpiod_line_get_value(gpioLine);          // :144 读当前电平
if (value < 0) {
    lg2::error("Failed to get value ...");           // :147-148 读失败记日志
} else {
    gpioHandleInitialState(value != 0);              // :152 non-zero → true
}
```

`gpioHandleInitialState()`（`gpioMon.cpp:112-126`）：

```cpp
void GpioMonitor::gpioHandleInitialState(bool value)
{
    if (auto itr = targets.find(value ? init_high : init_low);  // :114 按电平选 key
        itr != targets.end())
    {
        auto bus = sdbusplus::bus::new_default();
        for (const auto& tar : itr->second) {                   // :118 逐个启动
            auto method = bus.new_method_call(... StartUnit);   // :120-121
            method.append(tar, "replace");                      // :122
            bus.call_noreply(method);                           // :123
        }
    }
}
```

key 常量定义在 `gpioMon.cpp:21-22`（`init_high = "INIT_HIGH"`、`init_low = "INIT_LOW"`）。

### 5.3 三个容易踩的细节

1. **INIT 只在启动时生效**：`gpioEventHandler()` 查 `targets` 时只查 `rising`/`falling`（`gpioMon.cpp:72-87`），从不查 INIT 键——INIT 是"启动专用"的伪事件。
2. **选哪个 key 依据"裸电平 non-zero"**：`value != 0` → `INIT_HIGH`，否则 `INIT_LOW`（`gpioMon.cpp:114、152`）。配 polarity/ActiveLow 时要注意实际读到的是哪种语义。
3. **presence 路线有等价但不叫 INIT 的做法**：`multi-presence` 在 `requestGPIOEvents()` 里读一次当前值直接 `updateInventory(gpiod_line_get_value(gpioLine))`（`multi-presence/gpio_presence.cpp:201`），效果一样——"启动先把 inventory 对正，再等事件"。老版 presence 则在构造函数里调 `determinePresence()`（`presence/gpio_presence.hpp:89`），用 `libevdev_fetch_event_value` 读当前值（`presence/gpio_presence.cpp:60-79`）。

> Java 类比：INIT 机制就像 Spring 应用在 `@PostConstruct` 里先做一次"全量对账"，把数据库状态和外部系统状态同步一次，之后才靠事件做增量更新。

---

## 6. mapper 反查：为什么不把 D-Bus 服务名写死

presence 路线要把结果写进 inventory，但"提供 inventory 的那个 D-Bus 服务叫什么名字"**不是写死的**——因为机器层可以重定向服务名。代码用 `ObjectMapper` 反查。

### 6.1 `getService()` 逐行拆解（multi 版 `multi-presence/gpio_presence.cpp:28-52`，presence 版几乎一样 `presence/gpio_presence.cpp:34-58`）

```
常量定义：                                      multi-presence/gpio_presence.cpp:21-26
  INVENTORY_PATH = "/xyz/openbmc_project/inventory"            :21
  INVENTORY_INTF = "xyz.openbmc_project.Inventory.Manager"    :22
  MAPPER_BUSNAME = "xyz.openbmc_project.ObjectMapper"          :24
  MAPPER_PATH    = "/xyz/openbmc_project/object_mapper"       :25
  MAPPER_INTERFACE = "xyz.openbmc_project.ObjectMapper"       :26

getService(path, interface, bus):
  ├─ new_method_call(MAPPER_BUSNAME, MAPPER_PATH, MAPPER_INTERFACE, "GetObject")  :31-32
  ├─ append(path)                                    :34  ← 传"我要找哪个对象"
  ├─ append(vector<string>{interface})               :35  ← 传"它要实现哪个接口"
  ├─ bus.call(mapperCall) 读回 map<busname, paths>    :40-41
  ├─ catch 异常 → 打日志 + elog<InternalFailure>      :43-49
  └─ return mapperResponse.begin()->first            :51  ← 取 map 的第一个 key = 服务名
```

### 6.2 反查到服务名后怎么写 inventory

`multi-presence/gpio_presence.cpp:73-99`：

```
updateInventory(present):
  ├─ getObjectMap(present)                    :75 拼 D-Bus 对象树（见下）
  ├─ info 日志 "Updating inventory present..." :77-79
  ├─ bus = new_default()                      :81
  ├─ invService = getService(INVENTORY_PATH, INVENTORY_INTF, bus)  :82 ← 反查
  ├─ new_method_call(invService, INVENTORY_PATH, INVENTORY_INTF, "Notify") :85-86
  ├─ append(objectMap)                        :87
  └─ bus.call(invMsg)（不等返回，但这里用的是 call 会等回执）:90
       catch → 打日志 + elog                   :92-98
```

`getObjectMap()`（`multi-presence/gpio_presence.cpp:54-71`）拼出的结构（这就是 `ObjectMap` 类型，定义见 `multi-presence/gpio_presence.hpp:37-46`）：

```
inventory 路径（来自 JSON 的 Inventory 字段）
  └─ interface "xyz.openbmc_project.Inventory.Item"
       ├─ "Present"    = present  (bool)      :60
       ├─ "PrettyName" = name     (string)    :61
       └─（额外接口来自 ExtraInterfaces，逐个 emplace 空 PropertyMap）:64-67
```

> Java 类比：`getService()` 就是**服务发现**（Spring Cloud 里从 Eureka/Nacos 按"接口名+路径"查提供者的实例地址），而不是硬编码 `http://InventoryService:8080`。这样机器层把 inventory 服务改名/重定向时，代码不用动。`ObjectMap` 则像一个嵌套的 `Map<Path, Map<Interface, Map<Property, Value>>>` 的 DTO，`Notify` 是把整个 DTO 推给对方的批量更新接口。

---

## 7. 老栈机制：evdev + sd-event 是怎么等的

新版讲完了，回头看老版（读存量机器代码必备）。

### 7.1 `phosphor-gpio-monitor` 的 main（`mainapp.cpp:14-81`）

```
main:
  ├─ CLI11 解析 -p/--path -k/--key -r/--polarity -t/--target -c/--continue  :26-37
  ├─ sd_event_default(&event) 创建事件循环对象                             :53-54
  ├─ 构造 Monitor(path, key, polarity, target, eventP, continueRun)        :64-65
  │    └─ 基类 Evdev 构造里 openDevice() + initEvDev() + registerCallback()
  │       （evdev.hpp:84-98）
  └─ while (!monitor.completed())                                          :69
       └─ sd_event_run(eventP.get(), -1)                                   :72  ← -1=永久等待
```

和 asio 版最大的区别：**这里是 main 自己写 `while` 循环调 `sd_event_run()`**，`-1` 表示"没有事件就一直阻塞等"。asio 版则是 `io.run()` 把循环封装掉了。

### 7.2 `Evdev` 基类做的三件事

`evdev.hpp:84-98` 的构造顺序：

1. **`openDevice()`**（`evdev.cpp:24-34`）：`open(path, O_RDONLY | O_NONBLOCK)` 打开 `/dev/input/eventX` 设备，拿 fd。非阻塞。
2. **`initEvDev()`**（`evdev.cpp:37-56`）：`libevdev_new_from_fd` 把 fd 包装成 libevdev 句柄，存进 `devicePtr`（智能指针，自定义 deleter 见 `evdev.hpp:41-49`）。
3. **`registerCallback()`**（`evdev.cpp:59-71`）：`sd_event_add_io(event, ..., fd, EPOLLIN, callbackHandler, this)`——把 fd 注册进 sd-event 循环，有 `EPOLLIN` 就读，回调是 `Monitor::processEvents`（默认参数 `monitor.hpp:51`）。

### 7.3 事件到了怎么处理

`monitor.cpp:21-28` 的 `processEvents()` 是静态回调，靠 `userData` 拿回 `Monitor*`（`:24`），然后调 `analyzeEvent()`（`:26`）。

`monitor.cpp:31-85` 的 `analyzeEvent()`：

```
while (rc >= 0):
  rc = libevdev_next_event(...)              :42-43  ← 一直读到没事件为止
  if rc < 0 → return                         :44-49
  if 是 EV_SYN/SYN_REPORT → continue         :53-56  ← 跳过同步帧
  if ev.code == key && ev.value == polarity  :57    ← 命中配置的键和极性
       → StartUnit(target)                   :61-71
       → 若 !continueAfterKeyPress → complete = true :73-78  ← 让 main 的 while 退出
       → return
```

> ⚠️ 代码里有个细节注释（`monitor.cpp:37-38`）：测试发现**不写成 while 循环会漏事件**——所以老版必须"一次回调把设备里攒的事件全读干净"。Java 类比：像消息队列消费者一次 `poll()` 要尽量把 batch 拉完，否则会丢消息。

---

## 8. presence 两版的内部机制差异

### 8.1 老版 `phosphor-gpio-presence`（evdev）

- main 解析 `--drivers`（`path,device` 对，`presence/main.cpp:26-61` 的 `getDrivers`）、`--extra-ifaces`（逗号分隔，`presence/main.cpp:121-132`）。
- 构造 `Presence` 时从环境变量读 `DRIVER_BIND_DELAY_MS`（`presence/gpio_presence.hpp:82-88`），并立刻 `determinePresence()` 读当前值上报一次。
- `analyzeEvent()`（`presence/gpio_presence.cpp:91-138`）：
  - **插入**（`ev.value > 0`）：先 `sleep(delay)` 再 `bindOrUnbindDrivers(true)` 再 `updateInventory(true)`（`:120-127`）——**顺序是"先 bind 驱动再上报在位"**。
  - **拔出**：先 `updateInventory(false)` 再 `bindOrUnbindDrivers(false)`（`:128-132`）——**反序**。
- `bindOrUnbindDrivers()`（`presence/gpio_presence.cpp:186-224`）：往 `<path>/bind` 或 `<path>/unbind` 写 device 名（`:188-215`）——本质是"往 sysfs 文件写字符串"，Java 类比就是写配置文件触发内核动作。

> 📌 插入/拔出的**顺序不同**是有讲究的：插入时硬件要时间初始化，所以 delay 后先让驱动接管再声明"在位"；拔出时先声明"不在位"再解绑，避免应用还在用就已经把驱动卸了。

### 8.2 新版 `phosphor-multi-gpio-presence`（libgpiod）

- main 读 JSON（`multi-presence/main.cpp:30-179`），每条线造一个 `GpioPresence`（`:172-174`），`io.run()`（`:176`）。
- `requestGPIOEvents()`（`multi-presence/gpio_presence.cpp:153-207`）：申请线 → 拿 fd → 拼 flags 日志（BIAS/ACTIVE_LOW，`:172-196`）→ `assign(fd)` → **先 `updateInventory(当前值)`**（`:201`）→ `scheduleEventHandler()`。
- `gpioEventHandler()`（`multi-presence/gpio_presence.cpp:128-151`）：读事件 → 判 RISING=在位（`:139-147`）→ `updateInventory(...)` → 重新挂等待（`:150`）。
- **没有 bind/unbind 驱动的逻辑**：新版只管上报 inventory，不管内核驱动绑定（对比 `multi-presence/gpio_presence.cpp` 全文没有 `bindOrUnbindDrivers`）。
- 构造函数支持**移动语义**（`multi-presence/gpio_presence.hpp:77-89`）：`GpioPresence(GpioPresence&&)` 里先 `cancelEventHandler()`、移动 fd、再 `scheduleEventHandler()`——因为 main 用 `vector<GpioPresence>`（非指针，`multi-presence/main.cpp:65`）装对象，`push_back` 扩容时元素会被移动，必须正确处理 asio descriptor 的迁移。

> Java 类比：这相当于你的对象实现了"可安全转移"的语义（类似把 `CompletableFuture` 的引用从一个容器挪到另一个容器时，不能让回调丢失）——C++ 里就得手写移动构造函数。

---

## 9. 4 个进程机制对比总表

| 维度 | gpio-monitor（老） | multi-gpio-monitor（新） | gpio-presence（老） | multi-gpio-presence（新） |
|---|---|---|---|---|
| 入口 main | `mainapp.cpp:14` | `gpioMonMain.cpp:38` | `presence/main.cpp:63` | `multi-presence/main.cpp:30` |
| 配置来源 | 命令行 `-p/-k/-r/-t` | JSON `-c` | 命令行 | JSON `-c` |
| 线来源 | `/dev/input` + key code | libgpiod `gpiod_line_find` | `/dev/input` + key code | libgpiod `gpiod_line_find` |
| 事件库 | libevdev | libgpiod | libevdev | libgpiod |
| 循环 | `sd_event_run()` while（`mainapp.cpp:69-78`） | `io.run()`（`gpioMonMain.cpp:196`） | `sd_event_run()` while（`presence/main.cpp:149-158`） | `io.run()`（`multi-presence/main.cpp:176`） |
| 核心处理类 | `Monitor`（`monitor.hpp:25`） | `GpioMonitor`（`gpioMon.hpp:23`） | `Presence`（`presence/gpio_presence.hpp:34`） | `GpioPresence`（`multi-presence/gpio_presence.hpp:35`） |
| 事件处理函数 | `analyzeEvent`（`monitor.cpp:31`） | `gpioEventHandler`（`gpioMon.cpp:39`） | `analyzeEvent`（`presence/gpio_presence.cpp:91`） | `gpioEventHandler`（`multi-presence/gpio_presence.cpp:128`） |
| 终点动作 | StartUnit target | StartUnit target/targets | inventory + bind 驱动 | inventory |
| 启动状态恢复 | 无（靠 continue 标志） | `gpioHandleInitialState` INIT（`gpioMon.cpp:112`） | `determinePresence`（`presence/gpio_presence.hpp:89`） | `updateInventory(当前值)`（`multi-presence/gpio_presence.cpp:201`） |
| 服务名获取 | 不查（固定 systemd1） | 不查（固定 systemd1） | mapper `getService`（`presence/gpio_presence.cpp:34`） | mapper `getService`（`multi-presence/gpio_presence.cpp:28`） |

---

## 10. 排错速查表（机制视角）

| 现象 / 疑问 | 机制层面怎么查 |
|---|---|
| 进程起来了但事件不触发 | 确认 `io.run()` 前确实 `requestGPIOEvents()` 成功（`gpioMon.cpp:128-164`）；看 `Failed to request/get fd` 日志（`:133、140`） |
| 只触发一次就不动了 | `Continue` 没配 true → `gpioEventHandler` 结尾没重新挂（`gpioMon.cpp:102-109`） |
| asio 报 `operation_aborted` | 新版 presence 的回调专门识别它并静默返回（`multi-presence/gpio_presence.cpp:108-112`）——通常是 cancel 导致的，正常 |
| 老版漏事件 | 检查 `analyzeEvent()` 的 while 读空循环还在不在（`monitor.cpp:39-43`，注释说不加会漏） |
| BMC 重启后状态不对 | 查 INIT 配置（`gpioMon.cpp:112-126、144-153`）或 presence 的启动上报（`multi-presence/gpio_presence.cpp:201`） |
| inventory 服务名难道写死了？ | 没有，`getService()` 经 mapper `GetObject` 反查（`multi-presence/gpio_presence.cpp:28-52`） |
| Notify 失败 | 看 `Error in inventory manager call` 日志（`multi-presence/gpio_presence.cpp:92-98`）和 mapper 调用是否异常（`:43-49`） |
| 编译时报 asio 线程相关错误 | `meson.build:37-41` 强制 `-DBOOST_ASIO_DISABLE_THREADS`，单线程模型别用多线程 API |
| `gpioMon.cpp` 和 `mainapp.cpp` 谁是谁 | 以 `meson.build:110-133` 为准：`mainapp.cpp`→老 monitor，`gpioMonMain.cpp`→新 monitor（ONBOARDING 表格写反了） |
| 想确认边沿方向判断 | RISING 走 `targets["RISING"]`，FALLING 走 `targets["FALLING"]`（`gpioMon.cpp:72-87`）；presence 里 RISING=在位（`multi-presence/gpio_presence.cpp:147`） |
| 想给新进程加一种事件类型 | 在 `polarityMap`（`gpioMonMain.cpp:18-24`）加映射，并在 JSON 里用该字段；不用改事件循环 |

---

## 11. 一句话总结

> **新栈两个进程（multi-*）：main 读 JSON → 每条线造一个对象 → 构造时 `requestGPIOEvents()` 拿 fd 交给 asio → `async_wait` 回调里 `gpioEventHandler` 读事件 → 要么 `StartUnit` 拉 systemd target（monitor），要么经 mapper 反查服务名后 `Notify` 写 inventory（presence）→ 按 Continue 决定是否重新挂等待。老栈两个进程用 evdev + `sd_event_run()` 手动循环做同样的事。启动时的 INIT 读电平 / 当前值上报，补上"BMC 重启期间没有边沿事件"的状态对账；mapper 反查保证不写死服务名；整个行为由 JSON 配置驱动，换平台只换配置不换代码。**

配套：业务场景看《GPIO监控业务详解.md》；代码结构看 `ONBOARDING.zh-CN.md`；可视化看 `业务流程交互图.svg`；字段原文看 `README.md:25-56、99-115`。
