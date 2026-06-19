---
title: 环境搭建：主机模拟双轨
description: 在 PC 上零硬件跑通 FreeRTOS 的两条路径——POSIX(Linux/macOS/WSL2)完整教程与 Windows MSVC 占位
---

# 环境搭建：主机模拟双轨

## 这章解决什么问题

这套教程不要求你手上有任何开发板。我们用 FreeRTOS 官方的 POSIX port 把整套内核搬到你 PC 上当一个普通进程跑,这样后面每一章里出现的队列、信号量、任务通知,你都能立刻 `git clone` 下来敲两行命令看到效果,而不是盯着 API 文档干想。这一章就是告诉你怎么把这套 host 模拟环境在你自己机器上搭起来、跑通第一个 blinky。

我们准备了两条轨。**POSIX 轨**覆盖 Linux、macOS 和 WSL2,用 GCC + CMake,是目前已经跑通、本教程所有示例默认依赖的一条,下面分三页一步步带;**Windows MSVC 轨**基于官方 `Demo/WIN32-MSVC`,在原生 Windows + Visual Studio 上跑,稳定性更好但还没落地,先留占位。

## TL;DR:急性子直接抄

如果你只想最快看到 blinky 滚起来、细节回头再说,下面这一坨命令从零到运行(前提:已在 x86_64 的 Linux/WSL 上,装了 GCC 11+ 和 CMake 3.15+):

```bash
git clone <repo-url> Tutorial_FreeRTOS
cd Tutorial_FreeRTOS
git submodule update --init                                         # 拉外层 submodule
git -C third_party/FreeRTOS submodule update --init FreeRTOS/Source  # 只拉内核,绝不加 --recursive
cd code/00-bootstrap
cmake -B build -DUSER_DEMO=BLINKY_DEMO -DNO_TRACING=1
cmake --build build
./build/posix_demo
```

看到 `Starting echo blinky demo` 稳定滚动就成功了,`Ctrl-c` 退出。每个命令在干什么、为什么那两个开关不能少、出了问题怎么排,往下读三页详解。

## POSIX 轨:一步步搭

- [认识 POSIX port 与准备环境](./01-posix-setup.md) —— POSIX port 是什么、为什么用它、x86_64 硬前提、工具链、拉取源码(含 submodule 那两个坑)
- [理解构建配置](./02-posix-build.md) —— `USER_DEMO`/`NO_TRACING` 两个命门开关、`heap_3`/`GCC_POSIX` 是什么、构建日志怎么读
- [运行、读输出与排坑](./03-posix-run.md) —— 跑起来、读懂 TX/RX/定时器的输出、stdio 缓冲坑、gdb 信号、整条轨踩坑速查表

## Windows MSVC 轨

::: warning 占位
这条轨基于官方 `Demo/WIN32-MSVC/WIN32.sln`,用 Visual Studio Community 跑 blinky,是官方书《Mastering the FreeRTOS Kernel》的主力平台、最稳的一条。待在 Windows 环境上落地后填充:克隆、用 VS 打开 sln、选择 blinky 配置、编译运行,预期是控制台窗口里的闪烁输出。
:::

## 关于踩坑

搭建和运行阶段的坑(ARM64 segfault、submodule、构建开关、stdio 缓冲、gdb 信号)各归各位,集中在 [运行与排坑](./03-posix-run.md) 末尾的速查表里。至于「这套 host 模拟和真 MCU 的系统性行为差异」——栈溢出检测失效、阻塞式 syscall 拖垮模拟器、ISR API 的 hang 陷阱等——属于另一类问题,收在 [仿真坑点](../../pitfalls/),建议进入具体内核机制前扫一眼建立预期。
