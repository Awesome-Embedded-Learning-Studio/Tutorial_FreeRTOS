---
title: 低功耗
description: configUSE_TICKLESS_IDLE 一打开内核空闲任务会走上哪条新路径、Tickless Idle 在真 MCU 上靠「停 tick + 低功耗定时器唤醒」到底怎么省电、为什么 host 模拟下这套机制能编能跑却测不出任何省电效果,以及真硬件上用 tickless idle 要当心的唤醒延迟和低功耗定时器精度两个坑
---

# 低功耗

## 这章解决什么问题

前面十章我们关心的全是「正确性」——任务怎么切、队列怎么传、信号量怎么同步、中断怎么处理。这一章换一个完全不同的轴:**功耗**。在电池供电的嵌入式设备里,CPU 每多醒一毫秒、每多跑一条指令,都是从电池里抠走的电量,FreeRTOS 为此提供了一个机制,叫 **Tickless Idle(无滴答空闲)**。这一章要搞清楚的是:这个机制在真 MCU 上到底怎么省电、它打开后内核的空闲任务会多走哪条路、以及——必须立刻诚实说清楚的——为什么在 host 模拟下我们只能讲清楚它的**机制**,却**测不出任何真实的省电效果**。

这一章是本教程最「边界感」的一章:功耗是我们在 [教学边界](../01_why-rtos/) 里早就声明为「host 模拟教不了」的三件事之一(另两件是实时性、中断延迟)。但这一章不是空的——我们把 tickless idle 的机制从头到尾拆给你看,让你彻底理解「空闲任务打开这个开关后会走上一条什么样的路径、真 MCU 的 port 在那条路径里干了什么」,这样等你移到真硬件上,你知道这个开关在做什么、该不该开、开了要当心什么。机制是真的、可移植的;省电数字是边界外的、本教程不给的。

## TL;DR:一句话结论

| 问题 | 结论 |
|------|------|
| **tickless idle 的开关是什么** | `configUSE_TICKLESS_IDLE=1`,它让内核空闲任务多走一段「算预计空闲(`prvGetExpectedIdleTime`)→ 判门槛(`configEXPECTED_IDLE_TIME_BEFORE_SLEEP`,默认 2 tick)→ 挂起调度器 → 调 `portSUPPRESS_TICKS_AND_SLEEP(预计空闲)`」的逻辑 |
| **真正省电的脏活在哪** | 在 port 宏 `portSUPPRESS_TICKS_AND_SLEEP` 里——真 MCU port 关 tick、执行 `WFI`、配低功耗定时器、醒来后补 tick;这套操作和具体 MCU 硬件死绑定,每个 port 自己实现 |
| **host 模拟下会怎样** | POSIX port 没实现这个宏(`FreeRTOS.h` 兜了个空宏),所以打开开关**能编能跑、机制在跑,但 tick 不会真的被停、也测不出任何省电效果**——这是设计如此,不是 bug |
| **demo 怎么证明这点** | 一个只睡大觉的 Worker 任务,每趟睡 1 秒 tick 差仍 ≈ 1000,证的就是「host 上 tick 没被停」 |
| **真硬件上要当心什么** | 两个坑:**唤醒延迟变长**、**低功耗定时器精度粗** |
| **这章给 dashboard 加增量吗** | 不加——系统几乎不出现长空闲窗口、host 上宏又是空壳,加进去无新东西只会增加风险 |

## 各小节

- [Tickless Idle 的机制](./01-tickless-idle-concept.md) —— CPU 空闲时功耗花在哪、Tickless Idle 的核心思想(没活儿就把 tick 停掉让 CPU 进低功耗)、`configUSE_TICKLESS_IDLE` 让空闲任务走上哪条新路径、`portSUPPRESS_TICKS_AND_SLEEP` 为什么在 host 上是个空壳
- [打开 configUSE_TICKLESS_IDLE 的独立 demo](./02-tickless-demo.md) —— 跟着仓库 `code/11_low-power/` 的 demo 走,用「每趟睡 1 秒 tick 差仍 ≈ 1000」的铁证,把「机制在跑、host 上 tick 没停」演成可观测的数据
- [真硬件两个坑与 dashboard 增量取舍](./03-real-hardware-and-dashboard.md) —— 真 MCU 上用 tickless idle 必踩的两个坑(唤醒延迟、低功耗定时器精度)、为什么这章不给 dashboard 脊柱加增量、全章小结

## 关于边界

这一章引用的「边界」有两个去处:横向的总原则——「host 模拟把功耗、实时性、中断延迟声明为边界外」——在 [教学边界](../01_why-rtos/) 里;纵向的细节——「POSIX port 的能力天花板」(ARM64 segfault、GDB 信号干扰、栈双重性这些同源话题)——收在 [仿真坑点](../../pitfalls/),建议和本章对照读。

上一章 [任务通知](../10_task-notifications/) 讲完了比信号量/队列更轻的单点通信原语;下一章 [调试](../12_debugging/) 我们离开功耗、回到实操主线——`configASSERT`/`vAssertCalled` 怎么抓第一现场、栈溢出检测为什么在 POSIX port 下不生效、`vTaskList` 和运行时统计怎么把内核状态摆给你看,那一章 host 模拟能给你的真实数据比这一章多得多。
