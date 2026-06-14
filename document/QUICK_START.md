---
title: 快速开始
---

# 快速开始

本教程无需任何真实 MCU，在 PC 上即可运行全部 FreeRTOS 内核示例。提供 POSIX 与 Windows MSVC 双轨。

## 前置要求

::: code-group

```text [POSIX 轨]
Linux / macOS / WSL2
GCC 工具链 (gcc)
GNU make
```

```text [Windows 轨]
Visual Studio Community (MSVC)
```

:::

## 三步跑起来

1. **克隆仓库** 并进入 `codes_and_assets/`
2. **编译 blinky**：详见 [环境搭建章](./tutorial/02_environment/)
3. **运行**：POSIX 轨执行 `posix_demo`，Windows 轨打开 `WIN32.sln`

::: tip 编译开关
`mainCREATE_SIMPLE_BLINKY_DEMO_ONLY` 控制最小 blinky 与完整 demo 两层结构，初学建议先从 blinky 开始。
:::

> ARM64 segfault、信号处理、GDB 配置等注意事项见 [环境搭建章](./tutorial/02_environment/) 与 [仿真坑点](./pitfalls/)。
