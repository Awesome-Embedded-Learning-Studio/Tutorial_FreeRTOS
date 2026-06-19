---
title: 队列管理
description: 用队列替代任务间裸全局变量——xQueueCreate/Send/Receive 的传送带语义、按值拷贝、阻塞读与超时、最新值 vs 一帧不丢两种相反语义,配阻塞超时独立 demo 与 dashboard 采集→队列→显示增量
---

# 队列管理

## 这章讲什么

上一章我们把 dashboard 从单任务拆成了 sensor 采集任务和 display 显示任务,但两任务之间传数据靠一根**裸全局变量**——单写者单读者时侥幸能用,一旦传结构体就会撕裂,而且全局变量本身不带「数据到了」的到达通知。这一章就是来拔掉这根隐患的:用**队列(QUEUE)**替代全局变量。队列是 FreeRTOS 里最通用的任务间通信原语,你可以把它理解成一根**线程安全的、带阻塞通知的环形传送带**——一头塞一头取,内核帮你管好并发,还帮你管好「取空了就睡着等」。三个核心 API `xQueueCreate` / `xQueueSend` / `xQueueReceive`、最值钱的阻塞读能力、最容易踩的「按值拷贝」语义,这一章挨个讲透。

## TL;DR:带走的几条结论

| 主题 | 一句话 |
|------|--------|
| 队列是什么 | 线程安全、带阻塞通知的环形传送带;内核管并发、管「空了就等」 |
| `xQueueCreate(长度, 单条大小)` | 第二个参数是**消息本身大小不是指针大小**;从堆里搭环形缓冲区,**进 scheduler 前创建好** |
| 按值拷贝 | 投递 `memcpy` 进、取出 `memcpy` 出,传的是副本不是指针——彻底解耦发送方/接收方内存所有权、天然消撕裂,代价是多一次拷贝 |
| `xQueueSend` 第三参数 | `portMAX_DELAY` 死等 / 有限超时 / `0` 立刻返回 `errQUEUE_FULL`;返回值**必须看**,否则默默丢消息 |
| `xQueueReceive` 阻读塞读 | 空队列上接收任务进 blocked **不占 CPU**,数据一来就被唤醒近实时消费——这是队列最值钱的能力 |
| 超时返回 | 等不到数据就在约定 tick 后醒来报告 `errQUEUE_EMPTY`,不会被永久卡死 |
| 两种相反语义 | **最新值** = 容量 1 + `xQueueOverwrite`(单槽最新值寄存器);**一帧不丢** = 容量 > 1 + `xQueueSend`。选哪个看数据是「最新重要」还是「每条重要」 |
| dashboard 增量 | sensor→队列→display,把上一章裸全局变量换成「最新值寄存器」语义的队列,治好撕裂、display 从定时轮询升级成事件驱动 |

## 各小节

- [队列的概念:传送带、按值拷贝、阻塞读](./01-queue-concepts.md) —— 这章解决什么、`xQueueCreate` 怎么搭传送带、消息为什么按值拷贝、`xQueueSend` 队列满三档语义、`xQueueReceive` 阻塞读与超时为什么最值钱
- [独立 demo:把阻塞与超时演给你看](./02-blocking-demo.md) —— `code/05_queues/` 里 Sender 周期投帧、Receiver 有限超时阻塞读,在真实输出里同时演「被唤醒消费」与「空窗超时返回」
- [dashboard 增量:把那根全局变量换成队列](./03-dashboard-queue.md) —— `code/dashboard/` 用容量 1 + `xQueueOverwrite` 的「最新值寄存器」语义把 sensor 采样发给 display,附本章小结

## 前后章

- 上一章 [任务管理](../04_tasks/) 把单任务拆成 sensor + display,埋下了「裸全局变量有撕裂隐患」的伏笔,本章正是来收口的
- 下一章 [软件定时器](../06_timers/) 把「到点采样」从 sensor 任务自己 `vTaskDelay` 掐表,换成内核的定时器服务任务统一驱动
- 「host 模拟 vs 真 MCU 在队列行为上的差异」(拷贝开销体感、队列满了的表现)收在 [仿真坑点](../../pitfalls/),建议和本章对照读
