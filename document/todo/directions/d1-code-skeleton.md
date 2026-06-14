---
title: D1 - 主机模拟代码骨架
---

# D1：主机模拟代码骨架

> 让 FreeRTOS 示例不依赖真 MCU，在 PC 上跑通。本方向的产出是所有章节可运行代码的来源。

**优先级分布**：

| P0 | P1 | 总计 |
|----|----| ---- |
| 3 | 4 | 7 |

---

## P0

### [P0] POSIX 轨 fork `main_blinky`

- **来源**：PLAN task 1
- **描述**：基于 `portable/ThirdParty/GCC/Posix` + `Demo/Posix_GCC`，fork `main_blinky` 作为首个可跑示例，make 出 `posix_demo`
- **涉及**：`codes_and_assets/posix_demo/`
- **验收**：`make && ./posix_demo` 在 Linux / macOS / WSL2 跑通 LED 闪烁（模拟输出）

### [P0] Windows MSVC 轨

- **来源**：PLAN task 1
- **描述**：基于 `Demo/WIN32-MSVC/WIN32.sln`，VS Community 跑通 blinky。Windows MSVC 是官方书主力平台，最稳
- **涉及**：`codes_and_assets/win32_demo/`
- **验收**：`WIN32.sln` 编译运行，控制台窗口闪烁

### [P0] 标注 POSIX port 已知问题

- **来源**：PLAN task 1 / 评审建议
- **描述**：在环境搭建章明确 POSIX port 的 x86 前提，以及 ARM64 Ubuntu 24.04 segfault 已知问题
- **涉及**：`document/tutorial/02_environment/`、`document/pitfalls/`

---

## P1

### [P1] Mock HAL + bridge task 架构

- **来源**：PLAN task 2
- **描述**：每个模拟外设跑独立 native pthread 并 block signals，经 POSIX IPC（`pipe` / `eventfd`）+ FreeRTOS semaphore 投递到 bridge task，走标准 FromISR 路径。**严禁从外部线程直调 FromISR**（POSIX port 会 hang/assert）
- **参考**：`alxhoff/FreeRTOS-Emulator`、`FreeRTOS-Plus-TCP NetworkInterface.c`
- **涉及**：`codes_and_assets/harness/`

### [P1] 模拟外设实现

- **来源**：PLAN task 2
- **描述**：
  - 模拟 UART（`posix_openpt` 或 stdin/stdout）
  - 按钮（键盘事件转中断注入）
  - 传感器（周期注入）
  - 非关键外设用被动 stub
- **涉及**：`codes_and_assets/harness/`

### [P1] 环境搭建文档

- **来源**：PLAN task 1
- **描述**：填写 02_environment 章，覆盖双轨搭建步骤、harness 用法、编译开关 `mainCREATE_SIMPLE_BLINKY_DEMO_ONLY`、ARM64 注意事项
- **涉及**：`document/tutorial/02_environment/`

### [P1] 渐进式仪表盘项目代码

- **来源**：PLAN task 5
- **描述**：多任务传感器仪表盘——采集任务 → 队列 → 处理任务 → 显示任务。每章加一个 FreeRTOS 原语，最终拼成完整应用。配 blinky 与 full 两层结构
- **涉及**：`codes_and_assets/dashboard/`、`document/tutorial/14_project-dashboard/`
