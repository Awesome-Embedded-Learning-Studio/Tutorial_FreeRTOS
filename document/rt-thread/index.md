---
title: RT-Thread 对比轨
description: 这轨是什么——把 FreeRTOS 的核心原语逐个映射到 RT-Thread 的对应物,给「已经会一个、想迁移到另一个」的读者;含一张总览表与两篇正文(原语映射大表、调度与配置哲学差异)
---

# RT-Thread 对比轨

## 这轨解决什么问题

前面整套教程我们都围着 FreeRTOS 转,把它的任务、队列、信号量、互斥量、事件组、定时器、内存一层层拆开讲透。但现实里你大概率不会一辈子只碰一个 RTOS——尤其在国内,RT-Thread 是另一条绕不开的线:它自带组件框架、设备驱动模型、Kconfig 配置体系,生态和 FreeRTOS 很不一样。所以这一轨不是再教你一遍 RT-Thread,而是给那些「FreeRTOS 已经用熟了、现在要接手或评估一个 RT-Thread 项目」的人一张**迁移地图**:每个你熟悉的 FreeRTOS 概念,在 RT-Thread 里叫什么、长什么样、哪里能一对一平替、哪里会踩到语义不一致的暗坑。

我们做这件事的前提是:你已经跟着前面十四章把 FreeRTOS 的内核机制理解透了。因为对比的前提是两边都懂,而这一轨默认 RT-Thread 那边你还没怎么碰过,所以我们站在 FreeRTOS 这边的已知,往 RT-Thread 那边的未知搭桥。反过来如果你是 RT-Thread 老手第一次碰 FreeRTOS,这张表倒过来读同样管用——原语对应关系是对称的。

## TL;DR:原语总览对照

下面这张表是整轨的索引,每个原语对应的详细映射和差异注解在 [原语映射大表](./01-primitive-mapping.md) 里逐条展开。最后一行特意标红,**任务通知是 FreeRTOS 独有、RT-Thread 无直接等价物**的原语,这是迁移时最容易卡住的点。

| 概念 | FreeRTOS | RT-Thread | 迁移难度 |
|------|----------|-----------|---------|
| 任务/线程 | `xTaskCreate` / `xTaskCreateStatic` | `rt_thread_create` / `rt_thread_init` | 易,签名思路一致 |
| 消息队列 | `xQueueCreate` / `xQueueSend` / `xQueueReceive` | `rt_mq_create` / `rt_mq_send` / `rt_mq_recv`(**可变长**) | 中,RT-Thread 另有定长邮箱 `rt_mb_*` |
| 邮箱(定长/指针) | ——(无独立原语) | `rt_mb_send` / `rt_mb_recv`(4/8 字节,零拷贝) | RT-Thread 独有概念 |
| 信号量 | `xSemaphoreCreate[Binary\|Counting]` | `rt_sem_create` / `rt_sem_init` | 易 |
| 互斥量 | `xSemaphoreCreateMutex`(优先级继承) | `rt_mutex_create` / `rt_mutex_init`(优先级继承) | 易,两者都有优先级继承 |
| 事件组 | `xEventGroupCreate` / `xEventGroupSetBits` | `rt_event_create` / `rt_event_send` | 中,事件集语义略有差异 |
| 软件定时器 | `xTimerCreate` / `xTimerStart` | `rt_timer_create` / `rt_timer_start` | 中,定时器回调上下文不同 |
| 内存管理 | `heap_1`~`heap_5` 五选一 | small / slab / memheap 三选一 + mempool | 中,配置哲学不同 |
| 任务通知 | `xTaskNotifyGive` / `xTaskNotifyWait` | **无直接等价**(最近似信号/事件) | **难,需重新设计** |
| 配置哲学 | `FreeRTOSConfig.h` 一堆 `config*` 宏 | `rtconfig.h` 由 Kconfig 生成 | 横向差异,见下 |

## 各文章

- [原语映射大表:每个 FreeRTOS API → RT-Thread API → 差异注解](./01-primitive-mapping.md) —— 把上面那张总览表逐条展开:任务、消息队列(含 RT-Thread 独有的邮箱)、信号量、互斥量、事件集、软件定时器、内存管理,以及那个没有直接对应物的任务通知,每个原语都配「FreeRTOS 怎么写 → RT-Thread 怎么写 → 哪里不一样」
- [调度与配置哲学差异](./02-scheduling-and-config.md) —— 跳出单个原语看宏观:两种调度模型、`FreeRTOSConfig.h` 的 `config*` 宏 vs RT-Thread 的 `rtconfig.h` + Kconfig、静态 vs 动态分配的默认习惯、`main` 在两边到底是谁

## 上下文

这轨独立于前面的章节脉络,你可以随时切进来。但它的每一条映射都假设你熟悉 FreeRTOS 那侧的原语——如果某个 FreeRTOS 原语你还没吃透,回去补对应章节再来:任务见 [任务管理](../tutorial/04_tasks/)、队列见 [队列管理](../tutorial/05_queues/)、定时器见 [软件定时器](../tutorial/06_timers/)、信号量见 [中断管理](../tutorial/07_interrupts/)、互斥量与优先级反转见 [资源管理](../tutorial/08_resources/)、事件组见 [事件组](../tutorial/09_event-groups/)、任务通知见 [任务通知](../tutorial/10_task-notifications/)、内存见 [堆内存管理](../tutorial/03_memory/)。

> 📌 API 核对说明:本轨引用的 RT-Thread API 均对齐 RT-Thread 官方《编程手册》的线程管理、IPC(邮箱/消息队列/信号量/互斥量/事件集)、软件定时器、内存管理章节。RT-Thread 版本间个别宏名(如优先级上限 `RT_THREAD_PRIORITY_MAX`)以你工程里的 `rtconfig.h` 为准。
