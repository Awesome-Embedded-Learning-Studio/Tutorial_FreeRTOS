---
title: 仿真 vs 真实 MCU 坑点
description: 这一卷的导航——把 host 模拟与真 MCU 的系统性行为差异收成三类(平台与架构、调度与同步、资源与回收),每条「现象 → 为什么 → 对策 → 真硬件上怎样」,配一张各原语在仿真 vs 真 MCU 的行为差异对照表
---

# 仿真 vs 真实 MCU 坑点

主机模拟是把双刃剑:零硬件门槛、`git clone` 完就能看到任务在跑,但它和真 MCU 之间存在**系统性**的行为差异——不是某个 bug,而是 POSIX port 实现决定的、搬不过去的差异。这一卷不教任何原语怎么用(那是前面各章的事),它的唯一职责是把这些差异摊开,让你在动手写第一个任务之前就建立预期:**哪些现象在 host 下根本看不到(比如栈溢出 hook 永远不触发)、哪些行为在 host 下要靠替身演(比如「中断」得用定时器回调假装)、哪些差异会让你的 host 测试结果不能直接推断真硬件**。我们把教学目标明确限定在 RTOS **内核机制**(调度 / 同步 / 通信 / 内存 / 调试),主动把实时性、中断延迟、功耗划在边界外——这三条 host 模拟根本不真实,教不了,我们不装懂。

## 这些坑按主题分了三类

下面那张「10 大陷阱速览」是这一卷的索引,每一条都展开在三篇文章里。我们按「坑的根源」把它们分了三类:**平台与架构**层面的坑(POSIX port 只认 x86_64、信号干扰 GDB),来自 port 的实现细节;**调度与同步**层面的坑(非实时、ISR API hang、阻塞式 syscall、多核新竞态),来自「把一个分时 OS 当成 RTOS 跑」这个根本张力;**资源与回收**层面的坑(栈溢出检测失效、vTaskDelete 回收延迟、忙等饿死、CPU 占满),来自内核对 pthread 栈和线程生命周期管理的够不着。最后一篇是张对照表,把每条原语在 host 和真硬件上的行为差异并列摆出来,方便你逐条核对预期。

## TL;DR:10 大陷阱速览

下面这张表是这一卷的入口。每条都展开在对应文章里,「定位」列告诉你这条坑属于哪一类、在哪个上下文最先撞上,点链接直接跳过去。搭环境和调试时最先撞上的几条加粗。

| # | 陷阱 | 一句话 | 定位 | 详解 |
|---|------|--------|------|------|
| 1 | **非实时** | PC 调度无确定性,timing jitter 不可信,host 上测的延迟不能推断真 MCU | 调度 | [02 § 非实时](./02-scheduling-and-sync.md#非实时timing-jitter-不可信) |
| 2 | **栈双重性** | `configCHECK_FOR_STACK_OVERFLOW` 在 POSIX port 失效,水位线探针也不可信 | 资源 | [03 § 栈溢出检测](./03-resources-and-recovery.md#栈溢出检测posix-port-下的盲区) |
| 3 | **ISR API hang** | 严禁从外部 pthread 直调 FromISR,必须经 port 跟踪的上下文(定时器回调/tick hook) | 调度 | [02 § ISR API hang](./02-scheduling-and-sync.md#isr-api-hang别从外部-pthread-直调-fromisr) |
| 4 | **系统调用不安全** | 阻塞式 syscall(read/accept/select)会拖垮整个模拟器,一个任务卡住全员受牵连 | 调度 | [02 § 阻塞式 syscall](./02-scheduling-and-sync.md#阻塞式-syscall-拖垮模拟器) |
| 5 | **ARM64 segfault** | POSIX port 在 ARM64 已知崩溃,这是 port 的架构天花板不是内核问题 | 平台 | [01 § ARM64 segfault](./01-platform-and-arch.md#arm64-segfaultposix-port-的架构天花板) |
| 6 | **CPU 占满** | 忙等任务需主动让出(vTaskDelay/taskYIELD),否则饿死所有低优任务 | 资源 | [03 § CPU 占满与饿死](./03-resources-and-recovery.md#cpu-占满与忙等饿死) |
| 7 | **GDB 信号干扰** | 调试时需 `handle SIGUSR1 SIG34 nostop noprint`,否则每步都断在调度信号上 | 平台 | [01 § GDB 信号干扰](./01-platform-and-arch.md#gdb-信号干扰被-sigusr1-和-sig34-刷屏) |
| 8 | **timing 需手动注入** | 传感器/按钮事件靠软件定时器回调或 tick hook 周期注入,host 没有真中断源 | 调度 | [02 § 模拟中断源](./02-scheduling-and-sync.md#模拟中断源timing-靠手动注入) |
| 9 | **多核新竞态** | 多核 PC 暴露单核 MCU 上隐藏的竞态,host 跑出的 bug 可能真硬件上反而复现不了 | 调度 | [02 § 多核新竞态](./02-scheduling-and-sync.md#多核新竞态host-反而更容易暴露竞态) |
| 10 | **vTaskDelete 上限** | 删除任务的资源(栈/TCB)在 host 下回收时机与真 MCU 不同,host 占的 pthread 栈更大 | 资源 | [03 § vTaskDelete 回收](./03-resources-and-recovery.md#vtaskdelete-的资源回收差异) |

## 各小节

- [平台与架构:ARM64 segfault、GDB 信号、x86_64 前提](./01-platform-and-arch.md) —— POSIX port 只认 x86_64 的根因(那段只适配 x86_64 的上下文切换汇编)、ARM64 一启动就 segfault 的对策、GDB 被 `SIGUSR1`/`SIG34` 刷屏怎么 ignore 掉
- [调度与同步:非实时、ISR API hang、阻塞 syscall、多核竞态](./02-scheduling-and-sync.md) —— host 是分时系统不是 RTOS,所以 timing 不可信;为什么不能从外部 pthread 直调 FromISR、怎么用定时器回调安全演中断;为什么一个阻塞式 syscall 会拖垮整个模拟器;多核 PC 为什么反而更容易暴露竞态
- [资源与回收:栈溢出盲区、vTaskDelete、忙等饿死、CPU 占满](./03-resources-and-recovery.md) —— 栈溢出检测在 POSIX port 为什么失效、水位线探针为什么不动;删除任务的资源回收在 host 和真 MCU 上时机有何不同;忙等任务怎么饿死别人、CPU 占满怎么破
- [原语行为对照表:仿真 vs 真 MCU](./04-primitive-behavior-diff.md) —— 把队列阻塞、信号量、定时器、任务通知、事件组、内存逐条列出来,host 上和真 MCU 上各是什么行为、能不能信、差异在哪

## 上下文

这一卷是横向参考,不属于任何一条原语章的学习路径。第一次进来建议在读完 [为什么需要 RTOS](../tutorial/01_why-rtos/)、搭完 [环境搭建](../tutorial/02_environment/) 之后扫一遍建立预期;之后每次学新原语时遇到「怎么在我机器上演不出某现象」,回头对一下这张 10 大陷阱表和[对照表](./04-primitive-behavior-diff.md)基本能定位是 host 边界还是真 bug。各原语章的「关于踩坑」段也都会链回这里——[03 内存](../tutorial/03_memory/) 的栈体量错觉、[07 中断](../tutorial/07_interrupts/) 的 ISR API hang、[12 调试](../tutorial/12_debugging/) 的栈溢出检测失效、[13 排错](../tutorial/13_troubleshooting/) 的 host 演不全的故障,根因都收在这一卷里。
