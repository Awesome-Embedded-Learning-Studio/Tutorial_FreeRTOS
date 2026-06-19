---
title: 认识 POSIX port 与准备环境
description: POSIX 轨第一页——FreeRTOS POSIX port 是什么、为什么用它、x86_64 硬前提、工具链与拉取源码
---

# 认识 POSIX port 与准备环境

> POSIX 轨第一页。这一页结束时,你手里会有一个源码齐全、可以开始构建的 FreeRTOS 工程。下一页 [理解构建配置](./02-posix-build.md) 讲怎么把它编译出来。

## 我们要跑起来的东西到底是什么

本教程所有示例都跑在一个叫 **POSIX port** 的东西上,所以动手前得先讲清楚它是什么,不然你照着敲命令心里没底。FreeRTOS 本质上是一套和 CPU 架构强相关的内核——任务切换、中断屏蔽这些底层操作都得靠汇编去摆弄具体硬件寄存器,所以官方为每一类平台(stm32、esp32、rp2040……)各提供一份 port。POSIX port 是其中很特殊的一份:它不针对任何 MCU,而是针对符合 POSIX 标准的操作系统,也就是你的 Linux、macOS、WSL2。

它干的事情可以这么理解:FreeRTOS 内核里每一个任务,被 POSIX port 映射成宿主机上的一个 pthread;FreeRTOS 那套由硬件定时器驱动的 tick,被换成了一个软件定时器在宿主机上周期触发;于是整套内核调度逻辑就在你 PC 上当一个普通进程跑起来了。对你写应用代码来说完全无感——你照常 `xTaskCreate`、`xQueueSend`,底下到底是 STM32 的真中断还是 pthread 的模拟,你不用关心。这就是本教程「零硬件门槛」的根基:不用买板子、不用装交叉工具链、不用接线,`git clone` 完就能看到任务在跑。

## 硬前提:POSIX port 只认 x86_64

这个必须先讲死,不然你换了台机器跑不起来会一头雾水。POSIX port 在任务切换那一下用了一段架构相关的汇编去篡改线程上下文,而这段汇编**只适配了 x86_64**。所以在 ARM64 的机器上——比如某些 Surface 设备自带的 ARM 版 WSL、Apple Silicon 上跑的 Linux 虚拟机、树莓派的 64 位系统——`./posix_demo` 一启动就 segfault,常常连 `main` 都进不去。动手前先确认架构:

```bash
uname -m            # 必须是 x86_64。若是 aarch64,这套 host 模拟对你关闭,见排坑章
```

⚠️ 如果你拿到的是 aarch64,别去折腾「能不能让它跑起来」——这是 port 的架构天花板,不是配置问题,换一个 x86_64 的环境(WSL 装个 x86 Ubuntu 镜像、或 UTM 里跑 x86 Linux)是唯一的路。这件事的来龙去脉我们收在 [仿真坑点](../../pitfalls/) 里展开。

## 工具链:GCC 和 CMake

POSIX 轨用 GCC 编译、CMake 组织构建,版本要求不高但有个下限。GCC 需要 11 或更新(老版本的某些行为会让 POSIX port 的线程局部存储出问题),CMake 需要 3.15 或更新(内核的 CMake 脚本用了较新的语法)。确认一下:

```bash
gcc --version       # 期望 11 或更新
cmake --version     # 期望 3.15 或更新
```

不齐的话,Linux/WSL 上一条命令补齐:`sudo apt-get install -y build-essential cmake`。本教程实测在 WSL2 的 Ubuntu、内核 `6.18.33.1-microsoft-standard-WSL2`、GCC 16.1.1 上跑通,供你对照。

## 把源码拉下来

FreeRTOS 内核是通过 git submodule 引进来的,而且钉死在官方 tag `202411.00` 上——这意味着你 clone 之后有**两步** submodule 初始化要做,缺一不可。先看命令,再解释为什么是两步、为什么第二条那么讲究:

```bash
git clone <repo-url> Tutorial_FreeRTOS
cd Tutorial_FreeRTOS
git submodule update --init                                         # ① 拉外层 FreeRTOS submodule
git -C third_party/FreeRTOS submodule update --init FreeRTOS/Source  # ② 只拉内核,不 recursive
```

为什么内核要单独再 init 一次?因为 FreeRTOS 仓库的结构是嵌套的:`third_party/FreeRTOS/FreeRTOS/Source` 本身**又是一个 submodule**,指向 `FreeRTOS-Kernel`,默认是空的。第①条命令只把外层 FreeRTOS 外壳拉下来并锁定到 `202411.00`,真正的内核源码(tasks.c、queue.c 这些)在第②条里才落地。

⚠️ 这里有一个绝对不能踩的雷:**第②条命令只 init `FreeRTOS/Source` 这一个,千万别加 `--recursive`**。原因是 FreeRTOS 主仓库的 `.gitmodules` 里挂了三十多个 FreeRTOS-Plus 的无关子模块——TCP 协议栈、AWS 集成、mbedtls、wolfSSL、glib、libslirp……一旦 `--recursive`,你的磁盘会被几个 GB 的、本教程用不上的代码淹没,而且大概率因为网络问题中途断掉,留下一堆半拉子目录。只 init 内核这一个,干净利落。

验证内核源码落地,看这两个文件在不在就行。它俩在,下一页的构建才有东西可编:

```bash
ls third_party/FreeRTOS/FreeRTOS/Source/CMakeLists.txt    # 内核的 CMake 支持
ls third_party/FreeRTOS/FreeRTOS/Source/tasks.c           # 内核源码本体
```

## 小结

到这里你手里有了钉死版本的 FreeRTOS 外壳、有了真正的内核源码,实验台也摆清楚了——x86_64 架构、GCC 11+、CMake 3.15+、源码就位。地基打好,下一页 [理解构建配置](./02-posix-build.md) 我们就把它编译出来,而且重点不是照抄命令,而是搞懂那条 `cmake` 命令上的两个开关到底在干什么。
