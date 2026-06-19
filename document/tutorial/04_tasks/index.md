---
title: 任务管理
description: xTaskCreate 的每个参数在干什么、优先级如何决定谁先跑、任务的 ready/running/blocked 状态机怎么转、vTaskDelete 怎么收尾、configMINIMAL_STACK_SIZE 怎么定——配一个多优先级任务 + 状态快照 demo,再把 dashboard 心跳拆成 sensor + display 两任务
---

# 任务管理

## 这章解决什么问题

上一章我们把堆内存摊开了,结论是**任务的栈和 TCB 都从堆里 `pvPortMalloc` 出来**——舞台后台搭好了。这一章正式登台:动手 `xTaskCreate` 把任务造出来,给它们排好优先级,然后站在一旁看调度器在它们之间挑挑拣拣。

为什么要拆任务?裸机 `while(1)` 是一条单线,你得自己用状态机把「采样、处理、显示」切成时间片轮着做,代码越写越像状态机面条;RTOS 把这三件事各自封装成一个**独立的、自己带主循环的任务**,调度器在每个时刻自动挑出「此刻最该跑的那个」交给 CPU。你只需告诉调度器「谁更重要」(优先级),剩下的它来。这一章就是把这套机制亲手跑起来、看清楚调度器到底在怎么挑。

## TL;DR:关键结论

| 主题 | 一句话结论 |
|------|-----------|
| `xTaskCreate` 六参数 | 任务函数(永不返回)/名字(调试用)/栈深度(单位是**字**)/参数/优先级/句柄输出;返回 `pdPASS`/`pdFAIL`(多半是堆不够) |
| 优先级调度 | 任何时刻调度器都让「ready 且优先级最高」的任务跑;同优先级默认时间片轮转;**0 留给 idle**,自建任务至少给 1 |
| 状态机 | ready/running/blocked/suspended/deleted 流转;**blocked 是「主动让出 CPU」的好状态**,善用 `vTaskDelay`/阻塞 API 是多任务并存的前提 |
| starvation 坑 | 高优先级任务既不让出 CPU、也不被事件阻塞,会饿死所有低优先级任务 |
| `vTaskDelete` | 销毁任务;POSIX port 下内存回收有**延迟**(拖到 idle 懒回收),真 MCU 更直接 |
| `configMINIMAL_STACK_SIZE` | 是 idle/timer 的默认栈深和常用基准;**真正的栈大小由 `xTaskCreate` 的 `usStackDepth` 决定**,用 `uxTaskGetStackHighWaterMark` 探峰值再收紧 |
| host 体量错觉 | 所有任务栈都虚胖到 128KB,真 MCU 上是几百到几千字 |

## 各小节

- [任务的概念与机制](./01-concepts.md) —— `xTaskCreate` 六参数逐个拆、抢占式优先级调度、ready/running/blocked/suspended 状态机、`vTaskDelete` 收尾与 POSIX port 回收延迟、`configMINIMAL_STACK_SIZE` 旋钮
- [独立 demo:多优先级任务 + 状态快照](./02-demo.md) —— 仓库 `code/04_tasks/`,三个不同优先级 Worker 先忙等后 `vTaskDelay`、最低优先级 Monitor 用 `uxTaskGetSystemState` 拍快照,把调度器和状态机在真实输出里看出来
- [dashboard 增量:拆成 sensor + display](./03-dashboard.md) —— 把 dashboard 单一心跳拆成 sensor 采集 + display 显示两任务(不同优先级),用裸全局变量临时过渡,观测堆随任务数下降;末尾小结

## 关于踩坑

「host 模拟和真 MCU 在任务行为上的差异」——栈双重性、`vTaskDelete` 回收延迟、忙等饿死——属于系统性的仿真坑点,收在 [仿真坑点](../../pitfalls/) 里,值得对照本章读一遍。下一章 [队列管理](../05_queues/) 我们正式进入任务间通信,把 dashboard 里那根不安全的裸全局变量 `g_xLatestSample` 换成队列。
