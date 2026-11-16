# GPIO 监控业务详解：这个仓库到底在替 BMC "感知"什么

> 面向"代码已经看懂、但不清楚业务在干嘛"的你。
> 本文**不重复讲代码结构**（那些看 `ONBOARDING.zh-CN.md` 和配套的《GPIO监控机制讲解.md》），而是回答三个问题：
> ① 真实服务器里它办什么事？② 为什么需要这些信号和状态？③ 每种操作对应什么业务动作？
>
> 讲法：先给你一个能想象的真实场景，再点出背后的业务概念，最后告诉你"对应代码里的哪一段"。
>
> 📌 路径说明：本文所有路径都相对仓库根目录 `phosphor-gpio-monitor/`。
> `gpioMonMain.cpp`、`gpioMon.cpp`、`mainapp.cpp`、`monitor.cpp`、`evdev.cpp` 在**根目录**；
> `presence/` 和 `multi-presence/` 是**子目录**，里面的文件会写成 `presence/main.cpp:63` 这种带目录前缀的形式。
> 所有 `文件:行号` 都已用 `sed -n` 逐段核对过。

---

## 0. 一句话说清业务

把你熟悉的 OpenUBMC 硬件管理心智搬过来：这个仓库里的 4 个守护进程，就是 BMC 的**"眼睛和神经末梢"**——它们**不走主机 OS**，直接盯着主板上的 GPIO 引脚（电平、按键、插拔检测脚），一旦有变化就做两件事之一：

1. **告诉 systemd："去把某个流程跑起来"**（比如"机箱被打开了，去触发断电/告警流程"）——这是 monitor 类；
2. **告诉 inventory："这个部件现在插着/拔了"**（更新 D-Bus 上的 `Present` 属性）——这是 presence 类。

**这就是它的全部业务：在主机够不着、OS 帮不上忙的时候，替 BMC 把物理世界的硬件变化翻译成系统能理解的动作和状态。**

对照你已知的两个角色（下一节第 6 章会展开）：

- x86-power-control 是**"手"**——去拉电源脚；
- state-manager 是**"大脑"**——审批、记账、编排 target；
- 本仓库是**"眼睛/神经末梢"**——感知"有人按了键、盖子被掀了、内存条被拔了"。

---

## 1. 先讲三个真实场景：这些守护进程在机房里到底感知什么

把这三个场景在脑子里过一遍，后面所有业务概念都从这些故事里长出来。

### 场景 A：机箱开盖检测（入侵检测）

```
① 机房里一台服务器的机箱侧板被拆开
   → 主板上的"机箱入侵检测开关"（chassis intrusion switch）被释放，
      GPIO 电平从高变低（或从低变高，取决于硬件极性）。

② BMC 上的 gpio-monitor 守护进程一直异步等着这根线的边沿
   → 事件到来，读到一个 FALLING/RISING 边沿
      （判断逻辑：gpioMon.cpp:50-57 按 event_type 打 Asserted/Deasserted 日志）。

③ 进程根据 JSON 配置里这根线对应的 Targets，去拉起 systemd target
   → 典型的是 obmc-chassis-hard-poweroff@0.target 之类的流程
      （SVG 交互图里就是这个例子：业务流程交互图.svg:44）。

④ 后续流程由 state-manager 编排的 target 树接管：告警上 Redfish、
   记日志、甚至强制断电，防止带电机箱被继续操作。
```

**业务价值**：运维不用盯着摄像头，BMC 自己知道"这台机器被人动过"。这是物理安全/合规要求的基本项。

### 场景 B：前面板电源按钮被按下

```
① 有人（或机械臂）在机箱前面板按了一下 POWER_BUTTON
   → 这根 GPIO 线（设备树里的线名 "POWER_BUTTON"，
      仓库自带配置里就用了它：phosphor-multi-gpio-monitor.json:4）出现边沿。

② monitor 进程被 async_wait 唤醒，读到边沿（gpioMon.cpp:43-57）。

③ 按配置拉起对应 target（比如 PowerButtonDown.service，
   README 的示例配置：README.md:63-70）。

④ 这个按键事件最终进入开机/关机状态机——
   但"按下去之后开不开机"是 state-manager + x86-power-control 的事，
   本进程只负责"把这次按键如实上报成一个 target 动作"。
```

> 📌 关键认知：**本进程不是去按电源键的那只"手"**（那是 x86-power-control），
> 它是**"听到有人按了键，然后喊一嗓子"的那个传感器**。

### 场景 C：热插拔部件的在位检测（DIMM / 电源 / PCIe 卡）

```
① 运维在机器运行时拔掉一个 PSU 或内存条
   → 该槽位的在位检测脚（presence detect pin）电平翻转。

② presence 类守护进程被唤醒：
   - 新版（multi-presence）：libgpiod 读到边沿 → gpioEventHandler
     （multi-presence/gpio_presence.cpp:128-151）；
   - 老版（presence）：evdev 键盘事件 → analyzeEvent
     （presence/gpio_presence.cpp:91-138）。

③ 判断"插着还是拔了"：
   - 新版：上升沿=在位，下降沿=不在位（multi-presence/gpio_presence.cpp:147）；
   - 老版：ev.value > 0 视为在位（presence/gpio_presence.cpp:119-123）。

④ 更新 inventory：往 D-Bus 的 Inventory.Manager 发 Notify，
   把对应对象路径的 Present 属性写成 true/false
   （multi-presence/gpio_presence.cpp:73-99、140-157）。

⑤ Redfish 上这个部件的 State 立刻从 Enabled 变成 Absent，运维在管理界面看到告警。
```

**这就是 presence 类的全部业务**：维护"硬件里到底插着哪些部件"这份**始终准确的账**。

> 📌 三个场景的共同套路：**电平变化 → 守护进程感知 → 翻译成一个系统动作（拉 target 或改属性）**。
> 记住这个套路，4 个守护进程都是它的变体。

---

## 2. 为什么非得让 BMC 旁路感知？（业务价值）

你可能会问：电源键、开盖、插拔这些事，OS 里不能直接看吗？为什么要 BMC 单独盯着？三个真实价值：

| 业务价值 | 场景 | 没有这套机制会怎样 |
|---|---|---|
| **带外管理**（最重要） | 主机 OS 死机、蓝屏、根本没装系统；机箱被拆时主机可能根本没通电 | OS 里的任何软件都跑不起来，你无法知道"盖子开了""电源被拔了" |
| **不依赖主机 OS 的"旁路感知"** | GPIO 线直接接到 BMC 的 GPIO 控制器，不经过主机 | 必须等主机 Boot 完、驱动加载完才能上报，的实时性和可靠性都没保障 |
| **可追溯 / 自动联动** | 半夜机盖被开、 PSU 被拔，要立刻联动告警或断电 | 只能等第二天人工巡检发现；无法自动触发保护流程 |

一句话：BMC 的价值就在"**主机越是不省心的状态，它越能帮上忙**"——而 GPIO 旁路感知是这条价值的**物理入口**。

### 2.1 点出概念：什么叫"旁路感知"和"带外管理"

- **带外管理（Out-of-Band, OOB）**：管理通道（BMC 的网口、D-Bus、GPIO）和主机的业务通道（跑业务的 x86 网口/CPU）是**两套独立的硬件**。主机死了，管理通道还活着。
- **旁路感知**：不走主机的 I²C、不走主机的 ACPI 事件，而是 BMC **自己的 GPIO 控制器**直接接一根线到那个开关/检测脚。代码里体现就是 `gpiod_line_find("POWER_BUTTON")`（`gpioMonMain.cpp:122-124`）——BMC 按**设备树里的线名**找到这根线，完全绕开主机。

> 📌 对照 OpenUBMC：你在 openUBMC 里做过硬件监测的话，这一层就相当于"传感器直接接到 BMC 侧"，而不是从主机侧读。

### 2.2 两条技术路线：为什么同一个仓库里有 evdev 和 libgpiod 两套

这是新人最容易懵的地方，业务上这么理解：

| 路线 | 业务背景 | 事件来源 | 代表进程 |
|---|---|---|---|
| **老路线（evdev / gpio-keys）** | 早期内核把 GPIO 按键注册成 `/dev/input/event*` 键盘设备，BMC 读"按键事件" | `libevdev_next_event`（`monitor.cpp:42-43`） | `phosphor-gpio-monitor`、`phosphor-gpio-presence` |
| **新路线（libgpiod）** | 新平台直接把 GPIO 当 GPIO 用，按线名找线、读边沿，配置更灵活 | `gpiod_line_event_read_fd`（`gpioMon.cpp:43-44`） | `phosphor-multi-gpio-monitor`、`phosphor-multi-gpio-presence` |

业务结论：**新路线能"一个进程配多条线、按线名找线、靠 JSON 改行为"**（README.md:18-23），所以新平台优先用它；老路线是读存量机器代码时才遇到的。

---

## 3. 必懂的业务概念（先场景，后术语）

### 3.1 电平和边沿：RISING / FALLING / BOTH

回顾场景 A：盖子被拆，线从高变低——这是一个"边沿"，不是一个持续状态。代码把配置的字符串映射成 libgpiod 的边沿类型：

| 配置值 | 业务含义 | 代码映射 |
|---|---|---|
| `FALLING` | 只在"高→低"时触发（适合"按下即接地"的按键、入侵开关） | `gpioMonMain.cpp:20` |
| `RISING` | 只在"低→高"时触发（适合"插入即拉高"的在位脚） | `gpioMonMain.cpp:22` |
| `BOTH` | 两个方向都触发（适合要同时知道"插上"和"拔掉"的槽位） | `gpioMonMain.cpp:24`；不配时默认也是 BOTH（`gpioMonMain.cpp:84-86`） |

Java 类比：`EventMon` 就像给监听器注册的事件类型掩码（`$("btn").on("click", handler)` 里的 `"click"`），只不过这里是硬件电平的上升/下降沿。

### 3.2 极性和偏置：ActiveLow / Bias

场景 A 里"盖子被拆，电平从高变低"还是"从低变高"，取决于硬件怎么接。这就是业务上要配极性的原因：

- **`ActiveLow: true`**：信号在**低电平**时算"有效/ asserted"。代码里给请求标志加上 `GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW`（`gpioMonMain.cpp:186-190`）。
- **`Bias`**：配置内部上下拉，让悬空的检测脚有确定电平，可选 `AS_IS / DISABLE / PULL_UP / PULL_DOWN`（`gpioMonMain.cpp:26-34` 的 `biasMap`，解析在 `gpioMonMain.cpp:171-184`）。比如 PSU 在位脚常配 `PULL_UP`（README.md:132）。

> 📌 业务上这叫"硬件语义到软件语义的翻译层"：同样的物理接线，靠这两个字段决定"到底哪边算插入/按下"。

### 3.3 一次事件可能要拉多个 target：`Target` vs `Targets`

场景 B 里"电源键按下"可能不只是开机，还要记日志、点灯、报事件。所以配置支持两种写法（README.md:40-47）：

| 字段 | 语义 | 何时触发 | 代码 |
|---|---|---|---|
| `Target` | 单个 unit，**任何事件**来了都拉它一次 | 每个边沿都执行 | `gpioMon.cpp:59-69` |
| `Targets` | 一个 map，按 `RISING` / `FALLING` / `INIT_HIGH` / `INIT_LOW` 分组，每组一列 unit | 按边沿方向选对应组 | `gpioMon.cpp:71-87` |

Java 类比：`Target` 像一个"全局监听器"（所有事件都进它）；`Targets` 像一个 `Map<EventType, List<Handler>>` 的路由器，按事件类型分发到不同的处理器列表。

### 3.4 启动时的"状态恢复"：INIT_HIGH / INIT_LOW

这是最容易被忽略、但业务上很关键的一个概念。回到场景 C：BMC 自己重启了（比如固件升级后），**重启期间机箱盖一直是开着的、PSU 一直是插着的**——这时候根本不会产生新的边沿事件，怎么办？

- 代码在进程刚起来、注册事件监听之前，先读一次当前电平（` gpioMon.cpp:144-153`）；
- 如果 non-zero 就查 `Targets["INIT_HIGH"]`，否则查 `Targets["INIT_LOW"]`，把里面列的 target 先拉起来（`gpioMon.cpp:112-126`）；
- 这样"上电前就处于某状态"的设备，状态能被正确恢复，不会一直停在旧状态。

> 📌 业务结论：INIT 机制解决的是"**BMC 重启时的状态对账**"问题——没有它，BMC 重启后 inventory/状态会和设备真实状态对不上。

---

## 4. 4 个守护进程分别干什么业务

仓库里其实是 4 个独立编译的守护进程（`meson.build:110-136`）。先给你一张"业务分工表"：

| 可执行程序 | 源文件（真实构建关系） | 底层技术 | 业务职责 | 事件发生后做什么 |
|---|---|---|---|---|
| **`phosphor-gpio-monitor`** | 根目录 `mainapp.cpp`（`meson.build:110-116`） | evdev + sd-event | 老版单线监控：命令行指定一根 GPIO | 匹配到按键值 → 拉一个 target（`monitor.cpp:57-71`） |
| **`phosphor-multi-gpio-monitor`** | 根目录 `gpioMonMain.cpp` + `gpioMon.cpp`（`meson.build:118-133`） | libgpiod + boost::asio | 新版多线监控：JSON 配多条线 | 按边沿拉 `Target`/`Targets`（`gpioMon.cpp:59-100`）+ INIT 恢复 |
| **`phosphor-gpio-presence`** | 子目录 `presence/main.cpp` + `presence/gpio_presence.cpp`（`presence/meson.build:1-10`） | evdev + sd-event + mapper | 老版在位检测 | 更新 inventory `Present` + bind/unbind 驱动（`presence/gpio_presence.cpp:91-138、186-224`） |
| **`phosphor-multi-gpio-presence`** | 子目录 `multi-presence/main.cpp` + `multi-presence/gpio_presence.cpp`（`multi-presence/meson.build:1-17`） | libgpiod + boost::asio + mapper | 新版在位检测 | 更新 inventory `Present`（`multi-presence/gpio_presence.cpp:128-151`） |

一句话记住：

> **monitor 管"GPIO 变了要不要拉 systemd"；presence 管"设备插没插、上没上报 inventory"。**（`ONBOARDING.zh-CN.md:18`）

### 4.1 各进程的"启动入口"长什么样（帮你把进程和配置对上）

- `phosphor-gpio-monitor`：命令行参数 `-p/--path`、`-k/--key`、`-r/--polarity`、`-t/--target`（`mainapp.cpp:26-37`）；由 `phosphor-gpio-monitor@.service` 用环境变量 `${DEVPATH}/${KEY}/${POLARITY}/${TARGET}` 喂进去（`phosphor-gpio-monitor@.service:8`）。
- `phosphor-multi-gpio-monitor`：`-c/--config` 指向 JSON（`gpioMonMain.cpp:47-49`）；由 `phosphor-multi-gpio-monitor.service:7` 指向 `/usr/share/phosphor-gpio-monitor/phosphor-multi-gpio-monitor.json`。
- `phosphor-multi-gpio-presence`：同样 `-c/--config`（`multi-presence/main.cpp:39-41`）；由 `phosphor-multi-gpio-presence.service:8` 指向对应 JSON。
- `phosphor-gpio-presence`：命令行传 `--path/--inventory/--key/--name/--drivers`（`presence/main.cpp:75-96`），service 里还带 `mapper-wait` 依赖和 `ConditionPathExists`（`phosphor-gpio-presence@.service:3-5`）。

> ⚠️ **避坑**：`ONBOARDING.zh-CN.md:11-16` 的表格把两个 monitor 的"目录/实现"写反了（它说 `phosphor-gpio-monitor` 用 gpioMon.* + asio、`phosphor-multi-gpio-monitor` 用 mainapp.cpp）。**以 `meson.build:110-133` 为准**：`mainapp.cpp` 编译成 `phosphor-gpio-monitor`（evdev 老路径），`gpioMonMain.cpp + gpioMon.cpp` 编译成 `phosphor-multi-gpio-monitor`（libgpiod 新路径）。

---

## 5. 业务动作全景：一条 GPIO 事件最终变成了什么

把两条业务路线的"终点"说清楚：

### 5.1 monitor 路线：终点是 systemd target

```
GPIO 边沿
 → read fd 拿到 event_type（gpioMon.cpp:43-57）
 → 有 Target 就 StartUnit(target)（gpioMon.cpp:59-69）
 → 有 Targets 就按 RISING/FALLING 选一组逐个 StartUnit（gpioMon.cpp:71-100）
 → Continue=false 就退出进程（gpioMon.cpp:102-106）
   Continue=true 就重新挂异步等待，继续监控（gpioMon.cpp:108-109）
```

- 真正发出去的 D-Bus 调用是：`org.freedesktop.systemd1` 的 `Manager.StartUnit`，参数 `(unit, "replace")`（`gpioMon.cpp:15-17、63-68`）。
- `"replace"` 的业务含义：如果这个 target 正在跑，就替换掉重来——保证"最近一次硬件事件"优先。
- `call_noreply` = **发完不等回执**（`gpioMon.cpp:68`）——硬件事件不阻塞，Java 类比就是"异步 fire-and-forget 发送消息"。

### 5.2 presence 路线：终点是 inventory 的 Present 属性

```
GPIO 边沿
 → 判在位：上升沿=在位（multi-presence/gpio_presence.cpp:139-147）
 → getObjectMap 拼出 D-Bus 对象树：
     路径 = Inventory 字段
     接口 = xyz.openbmc_project.Inventory.Item
     属性 = Present=<bool>, PrettyName=<name>, 外加 ExtraInterfaces
     （multi-presence/gpio_presence.cpp:54-71）
 → getService() 经 mapper 反查 Inventory.Manager 的 bus name
   （multi-presence/gpio_presence.cpp:28-52、81-82）
 → 调 Notify 把对象树写进去（multi-presence/gpio_presence.cpp:84-99）
```

- 业务结果：Redfish 上这个部件的 `Present` / `State` 跟着变，运维立刻看到"卡被拔了"。
- 老版 presence 还多一步：**bind/unbind 内核驱动**——设备在位时把驱动绑上，拔出时解绑（`presence/gpio_presence.cpp:186-224`），并且可以配 `DRIVER_BIND_DELAY_MS` 延迟（`presence/gpio_presence.hpp:82-88`），给"设备上电后内核要等一会才能 bind"的硬件留缓冲。

---

## 6. 和 x86-power-control / state-manager 的业务分工（边界别混）

这是最容易混的一层，用你熟悉的微服务类比：

- **state-manager = 大脑（审批 + 记账 + 编排）**：决定"这台机器现在允许不允许开机"，维护权威状态，拉起 systemd target 树。
- **x86-power-control = 手（动手）**：真正去拉 PWRBTN 那根电源脚、盯 PWROK、把结果写回状态。
- **本仓库（gpio-monitor）= 眼睛/神经末梢（旁路感知）**：把"按键被按、盖子被开、设备被拔"这些**物理世界的变化**翻译成 target 动作或 inventory 状态。

一个"前面板电源键被按下"的完整业务链条：

```
① 人按下 POWER_BUTTON（物理动作）
② phosphor-multi-gpio-monitor 感知边沿（gpioMon.cpp:43-57）
   → StartUnit 配置的 Target（gpioMon.cpp:59-69）
③ systemd 按 target 树展开（编排在 .target/.service 文本里，不在 C++ 里）
④ 最终落到 state-manager 的状态机 + x86-power-control 的电源动作
⑤ inventory / Redfish 状态随之更新
```

看出分工了吗？**本进程只做第②步——"感知 + 喊一嗓子"**；第③④⑤步是大脑和手的事。

> 📌 所以你在这份代码里**看不到任何"判断该不该开机"的逻辑**——只有"该拉哪个 target"。业务上它是**输入传感器**，不是决策者。这正是 OpenBMC 松耦合的设计：感知、决策、执行三层各管各的。

---

## 7. 配置驱动：平台适配靠 JSON，不靠改代码

和你看过的 systemd 机制文档（《systemd之service与target.md》第 5 章）里的"配置驱动"思想一脉相承：要改行为，**改 JSON 文本文件即可，不用重新编译 C++**。

### 7.1 `phosphor-multi-gpio-monitor.json` 字段（monitor 路线）

| 字段 | 业务含义 | 是否必填 | 代码解析位置 |
|---|---|---|---|
| `Name` | 这根线的可读名字（用于日志） | 建议 | `gpioMonMain.cpp:78-79` |
| `LineName` | **设备树里的线名**（推荐，按名字找线） | 与 GpioNum/ChipId 二选一 | `gpioMonMain.cpp:121-125` |
| `GpioNum` / `ChipId` | GPIO 偏移 / gpiochip 号（备选，按编号找线） | 无 LineName 时必填 | `gpioMonMain.cpp:97-118` |
| `EventMon` | FALLING / RISING / BOTH（默认 BOTH） | 可选 | `gpioMonMain.cpp:136-148` |
| `Target` | 任意事件都拉的单个 unit | 可选 | `gpioMonMain.cpp:160-163` |
| `Targets` | 按 RISING/FALLING/INIT_HIGH/INIT_LOW 分组的 unit 列表 | 可选 | `gpioMonMain.cpp:165-169` |
| `Continue` | true=持续监控；不配=第一次事件后停止 | 可选 | `gpioMonMain.cpp:150-154` |
| `ActiveLow` | 低电平算有效 | 可选 | `gpioMonMain.cpp:186-190` |
| `Bias` | 内部偏置（AS_IS/DISABLE/PULL_UP/PULL_DOWN） | 可选 | `gpioMonMain.cpp:171-184` |

仓库自带示例（`phosphor-multi-gpio-monitor.json:1-12`）就配了 `POWER_BUTTON`（BOTH + Continue）和 `PowerGood`（ChipId=gpiochip0, GpioNum=14）两条线。README 里还有更完整的示例（README.md:60-83）。

### 7.2 `phosphor-multi-gpio-presence.json` 字段（presence 路线）

| 字段 | 业务含义 | 是否必填 | 代码解析位置 |
|---|---|---|---|
| `Name` | inventory 项的 `PrettyName` | 必填 | `multi-presence/main.cpp:135-143` |
| `LineName` | 设备树里的线名（推荐） | 与 GpioNum/ChipId 二选一 | `multi-presence/main.cpp:112-116` |
| `GpioNum` / `ChipId` | 按编号找线（备选） | 无 LineName 时必填 | `multi-presence/main.cpp:88-109` |
| `Inventory` | 要创建的 inventory 对象路径 | **必填** | `multi-presence/main.cpp:124-133` |
| `ExtraInterfaces` | 额外挂到该项上的接口列表 | 可选 | `multi-presence/main.cpp:166-170` |
| `ActiveLow` | 低电平算"在位" | 可选 | `multi-presence/main.cpp:160-164` |
| `Bias` | 内部偏置 | 可选 | `multi-presence/main.cpp:145-158` |

仓库自带示例（`phosphor-multi-gpio-presence.json:1-13`）配了 `DIMM A0` 和 `Powersupply 0`（后者带 `ActiveLow: true`、`Bias: PULL_UP`、额外接口 `xyz.openbmc_project.Inventory.Item.PowerSupply`）。

### 7.3 平台适配的真实做法

> 机器层通过 **bitbake bbappend** 替换 `phosphor-multi-gpio-monitor.json` / `phosphor-multi-gpio-presence.json` 完成平台适配（README.md:27-29、101-103）。

Java 类比：这就像把"哪些 GPIO 对应哪些动作"从硬编码的 `switch` 抽到了 Spring 的 `application.yml` 里——换一块板子，只换配置文件，不动一行 C++。

---

## 8. 常见业务故障场景（现象 → 硬件原因 → 代码怎么体现）

| 业务现象 | 可能的硬件/业务原因 | 代码里会看到什么 |
|---|---|---|
| 按了前面板电源键，系统没反应 | `LineName` 配错，或 `EventMon` 极性和实际接线相反 | 找不到线 → 日志 `Failed to find the GPIO`（`gpioMonMain.cpp:127-131`）；或事件方向不匹配 |
| 开盖告警不触发 | 入侵开关接的是另一根线，或该线没配 `Targets` | 有 `Asserted/Deasserted` 日志（`gpioMon.cpp:50-57`）但没有 StartUnit 调用 |
| BMC 重启后设备状态对不上 | 没配 `INIT_HIGH`/`INIT_LOW` | `gpioHandleInitialState` 查不到对应键（`gpioMon.cpp:112-126`） |
| Redfish 上 PSU 一直显示"在位"，实际已拔 | 在位脚配成了 `ActiveLow` 但硬件是高有效，或 Bias 不对 | inventory 的 `Present` 没翻转（`multi-presence/gpio_presence.cpp:147`） |
| 设备插上后内核驱动没起来 | `DRIVER_BIND_DELAY_MS` 太短，或没配 `--drivers` | bind 失败日志（`presence/gpio_presence.cpp:217-222`） |
| presence 服务起不来 | mapper/inventory 服务还没出现 | `phosphor-gpio-presence@.service:3-4` 的 `mapper-wait` 依赖没满足 |

---

## 9. 排错速查表（业务视角）

| 你想知道 / 遇到的问题 | 怎么查 |
|---|---|
| 系统里有哪些 gpiochip、线名对不对 | `gpiodetect`、`gpioinfo`、`gpiofind POWER_BUTTON`（"按线名找线"对应 `gpioMonMain.cpp:124`） |
| 事件到底来没来 | `journalctl -u phosphor-multi-gpio-monitor`，搜 `Asserted/Deasserted`/`monitoring started`（`gpioMon.cpp:52、155`） |
| 拉了哪个 target | journal 搜 `StartUnit` 的 unit 名，或 `systemctl status <target>` |
| inventory 的 Present 更新没更新 | `busctl get-property xyz.openbmc_project.Inventory.Manager <path> ... Present`；日志看 `Updating inventory present property`（`multi-presence/gpio_presence.cpp:77-79`） |
| 当前哪个进程在跑 | `systemctl list-units 'phosphor-*gpio*'`；4 个进程名字见第 4 章表格 |
| ONBOARDING 表格和代码对不上 | 以 `meson.build:110-136` 为准，见第 4.1 节的避坑说明 |
| 想加一个"开盖就告警"的业务 | 在 JSON 的 `Targets` 里给这根线配 `FALLING → obmc-...intrusion.target`，不用改 C++（README.md:43-47） |
| 改了 JSON 没生效 | 确认改的是 `/usr/share/phosphor-gpio-monitor/` 下的文件（service 里写死的路径：`phosphor-multi-gpio-monitor.service:7`），并重启服务 |

---

## 10. 一页速记（业务版）

- **它管什么**：BMC 的旁路"眼睛"——直接盯 GPIO 电平/按键/插拔，翻译成 systemd target 或 inventory 状态。
- **三个真实场景**：机箱开盖（入侵检测）、前面板电源键、部件在位检测（DIMM/PSU）。
- **为什么需要**：带外管理——主机死了/没 OS 时，只有 BMC 的旁路通道还能感知物理世界。
- **4 个进程**：两个 monitor（老 evdev 单线 / 新 libgpiod+JSON 多线）+ 两个 presence（老 evdev / 新 libgpiod）。
- **一个套路**：电平变化 → 感知 → 拉 target 或改 Present 属性。
- **INIT 恢复**：BMC 重启时读当前电平，按 INIT_HIGH/INIT_LOW 先拉 target，解决状态对不上（`gpioMon.cpp:112-126`）。
- **Target vs Targets**：前者任何事件都拉；后者按边沿方向分组拉（`gpioMon.cpp:59-87`）。
- **和大脑/手的分工**：state-manager 是大脑，x86-power-control 是手，本仓库是眼睛——只感知，不决策。
- **配置驱动**：平台适配靠 bbappend 换两个 JSON，不动 C++。

> 配套：代码结构看 `ONBOARDING.zh-CN.md`；4 个进程内部机制看《GPIO监控机制讲解.md》；业务流程可视化看 `业务流程交互图.svg`；字段原文看 `README.md:25-56、99-115`。
