---
title: 理解构建配置
description: POSIX 轨第二页——看懂构建命令上的 USER_DEMO/NO_TRACING 两个命门开关、heap_3 与 GCC_POSIX 是什么,以及构建输出怎么读
---

# 理解构建配置

> POSIX 轨第二页。[上一页](./01-posix-setup.md) 我们把源码和环境备齐了,这一页把它编译出来。重点不在敲命令——命令就两行——而在搞懂命令上那两个开关为什么是命门,以及构建日志里那些字眼(heap_3、GCC_POSIX)到底是什么意思。这样以后报错你才知道是哪一环出了问题。下一页 [运行与排坑](./03-posix-run.md) 让它真正跑起来。

## 构建命令:两步走

POSIX 轨的工程在仓库的 `code/00-bootstrap/` 目录,进去构建。配置和编译分两步,配置阶段带上两个开关:

```bash
cd code/00-bootstrap
cmake -B build -DUSER_DEMO=BLINKY_DEMO -DNO_TRACING=1
cmake --build build
```

命令本身平淡无奇,但 `-DUSER_DEMO=BLINKY_DEMO` 和 `-DNO_TRACING=1` 这两个开关,漏哪个炸哪个,所以我们花点时间把它们讲透。

## USER_DEMO:告诉 main.c 走哪个 demo

FreeRTOS 官方的 POSIX demo 其实准备了两套:简单的 `main_blinky`(就是我们这个,两个任务一个定时器)和完整的 `main_full`(一大堆压力测试)。`main.c` 里用一套条件编译在编译期决定跑哪套,逻辑是这样的——如果编译时定义了 `USER_DEMO` 这个宏,就用它的值当选择;如果没定义,就默认走 `FULL_DEMO`。

问题在于,本教程的最小工程**只编了 blinky 那套源,没编 `main_full.c`**。所以如果你忘了传 `-DUSER_DEMO=BLINKY_DEMO`,`main.c` 就默认去找 `main_full()`,链接阶段直接报 `undefined reference to 'main_full'`。换句话说,这个开关不是可选的优化,而是和「我们编了哪些源」配套的硬约束:你给 `BLINKY_DEMO`,它才走 blinky 分支,不去碰那个没编的 `main_full`。

## NO_TRACING:关掉我们用不上的 trace

第二个开关 `NO_TRACING=1` 对付的是另一件事。官方 demo 默认开着 trace(运行时追踪),对应 `projENABLE_TRACING=1`,这会去拉一个叫 FreeRTOS-Plus-Trace 的 submodule。而本教程为了保持最小,根本没初始化那个 submodule。所以如果不传 `NO_TRACING=1`,构建就会在找不到 `trcRecorder.h` 时报错。

这个开关干的就是把 `projENABLE_TRACING` 设成 0,跳过整条 trace 链路。trace 对学习内核机制本身没有帮助,关掉它既让工程更干净,也免得你为了一个用不上的功能去 init 多一个 submodule。

## heap_3 与 GCC_POSIX:看懂构建日志里的字眼

构建跑起来之后,日志里会出现两个词,这里提前讲清楚,免得你看着像天书。一个是 **heap_3**,它是 FreeRTOS 的内存管理方案之一——FreeRTOS 提供了 heap_1 到 heap_5 五种内存管理实现,`heap_3` 是对标准 `malloc`/`free` 的简单包装(加了线程安全),适合 POSIX 这种宿主机环境(宿主机本来就有成熟的 malloc)。这个选择写在我们 CMakeLists 的 `set(FREERTOS_HEAP "3")` 里。另一个是 **GCC_POSIX**,就是我们在 [上一页](./01-posix-setup.md) 讲的那个 POSIX port,标识当前用的是哪份平台适配代码。

## 构建输出走读

干净构建的输出大致长这样(路径中段我用 `...` 省略了,重点看进度和编译单元):

```
[  6%] Building C object .../freertos_kernel_port.dir/.../GCC/Posix/port.c.o
[ 13%] Building C object .../freertos_kernel_port.dir/.../GCC/Posix/utils/wait_for_event.c.o
[ 13%] Built target freertos_kernel_port
...
[ 53%] Building C object .../freertos_kernel.dir/tasks.c.o
[ 60%] Building C object .../freertos_kernel.dir/timers.c.o
[ 66%] Building C object .../freertos_kernel.dir/portable/MemMang/heap_3.c.o
[ 73%] Linking C static library libfreertos_kernel.a
[ 73%] Built target freertos_kernel
[ 80%] Building C object .../console.c.o
[ 86%] Building C object .../main.c.o
[ 93%] Building C object .../main_blinky.c.o
[100%] Linking C executable posix_demo
[100%] Built target posix_demo
```

读这份日志能看出构建分两层:先是 `freertos_kernel_port`(POSIX port 本身)和 `freertos_kernel`(内核的 tasks.c、queue.c、timers.c 这些)编成一个静态库 `libfreertos_kernel.a`,你能看到 `heap_3.c.o` 也在里面,就是上一节说的内存管理方案;然后才轮到我们的三个应用源(console.c、main.c、main_blinky.c)编译、链接上去,产出最终的可执行文件 `posix_demo`。全程在 `-Wall -Wextra -Wpedantic` 全开的情况下零 warning 零 error——如果这份日志在你的机器上冒出 warning,先停下来看,八成是工具链版本或源码状态有问题。

## 验证产物

构建完顺手确认一下产出物是个正经的 x86-64 可执行文件:

```bash
file build/posix_demo
```

期望看到 `ELF 64-bit LSB pie executable, x86-64`,带 debug info、没 strip。带 debug 这点很关键——它意味着你后面可以用 gdb 直接调试这个 demo(虽然 POSIX port 调试有些坑,见下一页)。

## 小结

这一页我们没写一行代码,但搞懂了构建命令在干什么:`USER_DEMO` 是和「编了哪些源」配套的硬开关,`NO_TRACING` 关掉用不上的 trace 链路,`heap_3`/`GCC_POSIX` 是内核内存方案和平台 port 的标识。理解了这些,构建对你来说就不是黑箱了。下一页 [运行与排坑](./03-posix-run.md) 我们让它真正跑起来,读它的输出,并收拾几个运行阶段才会冒出来的坑。
