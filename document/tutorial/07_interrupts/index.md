---
title: 中断管理
description: ISR 为什么不能阻塞、为什么只能用 *FromISR 系列 API,二值/计数信号量怎么做 ISR↔任务同步,延迟中断处理这套最经典模式长什么样,portYIELD_FROM_ISR 是干嘛的——以及 POSIX port 下「为什么不能从外部 pthread 直调 FromISR」这个 host 模拟最大的坑,我们怎么用软件定时器回调安全模拟中断源
---

# 中断管理

## 这章解决什么问题

前面几章讲的所有原语——任务、队列、软件定时器——都跑在一个共同前提上:它们都是**任务上下文**里的东西。但嵌入式系统里还有另一类完全不同的执行上下文:**中断服务程序(ISR)**——串口收字节、定时器溢出、按键按下,硬件都会异步打断 CPU 跳进你写的 ISR 跑一段。这一章要回答:ISR 这个特殊上下文和任务上下文到底有什么不一样?ISR 里能用哪些 API、不能用哪些?ISR 想把「事件发生了」告诉一个任务、让任务去处理,最干净的做法是什么?

这也是整个教程里 host 模拟最棘手、最需要「诚实交代边界」的一章,因为真相是:**POSIX port 没有真正的中断**。所以本章双线并行:一边把真硬件上 ISR 的规矩、中断↔任务同步的经典模式讲透(这部分 host 和真硬件逻辑完全一致);一边诚实讲清 host 模拟下「用什么安全的办法假装有中断」,以及为什么不能瞎假装(从外部 pthread 直调 FromISR 会直接 hang)。把这两条分清,你带走的就是一套「搬到真硬件上一行不用改」的中断处理写法。

## TL;DR:中断管理的核心要点

| 要点 | 一句话 |
|------|--------|
| **ISR 必须极快返回** | 只做最必要的事立刻返回,长活丢给任务做——这就是延迟中断处理(deferred interrupt handling) |
| **ISR 绝不能阻塞** | 不能 `vTaskDelay`、不能死等队列、不能拿互斥量(ISR 没有任务状态可挂、没有优先级可继承) |
| **ISR 只能用 `*FromISR` API** | `xSemaphoreGiveFromISR`/`xQueueSendFromISR`…任务版函数的上下文检查对 ISR 既不安全又无意义 |
| **二值信号量** | ISR `xSemaphoreGiveFromISR` 给一个 token、处理任务 `xSemaphoreTake` 死等被唤醒,这就是「事件来了」最经典的通知工具 |
| **`portYIELD_FROM_ISR`** | 中断退出那一刻按需切到被唤醒的高优先级任务,配合 `xHigherPriorityTaskWoken` 出参三步走 |
| **⚠️ host 最大的一颗雷** | 不能从外部 pthread 直调 `*FromISR`,调了会 hang;安全模拟中断源要用**软件定时器回调**或 **tick hook** 这两个 port 跟踪的上下文 |

## 各小节

- [中断的概念:ISR 与任务上下文、FromISR API、host 模拟的雷](./01-concepts.md) —— ISR 的三条硬规矩、二值/计数信号量、`portYIELD_FROM_ISR` 三步走,以及「为什么不能从外部 pthread 直调 FromISR、怎么安全模拟中断源」
- [独立 demo:用定时器回调模拟中断源,经二值信号量唤醒处理任务](./02-deferred-isr-demo.md) —— 跟着 `code/07_interrupts/` 把延迟中断处理整套演出来,完整走一遍 give/take + `portYIELD_FROM_ISR` 骨架
- [dashboard 增量:模拟按钮中断,经信号量触发 sensor 立即采样](./03-dashboard-button-irq.md) —— 给 dashboard 加「按钮中断」唤醒源,讲清两个 host 模拟的稳妥取舍(省掉 yield、用旁路探询替代队列集)

## 关于踩坑

本章 host 模拟特有的坑——「从外部 pthread 直调 FromISR 会 hang」「定时器服务任务里 FromISR + portYIELD_FROM_ISR 偶发 stall」「队列集等待会 stall」——根因都是 POSIX port 的中断嵌套计数和信号调度只认它自己跟踪的上下文。这些在[仿真坑点](../../pitfalls/)的「ISR API hang」条里有专门记录,务必对照本章读一遍。上一章是[软件定时器](../06_timers/)(我们用它扮演 ISR);下一章[资源管理](../08_resources/)我们离开中断、进入另一类同步问题——互斥量、优先级反转、优先级继承,你会发现二值信号量和互斥量长得几乎一样,但有一个关键差别(优先级继承)。
