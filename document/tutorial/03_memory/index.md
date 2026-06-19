---
title: 堆内存管理
description: FreeRTOS 的五种堆实现 heap_1~heap_5 怎么选、configTOTAL_HEAP_SIZE 和 xPortGetFreeHeapSize 在干什么、为什么内存在任务之前——概念 + 独立 demo + dashboard 监控增量三篇文章的导航页
---

# 堆内存管理

## 这章解决什么问题

上一章我们把环境搭起来、跑通了第一个 blinky,但有一个问题一直被刻意绕开——那些任务、队列、定时器到底住在内存的哪儿?你 `xTaskCreate` 的时候并没有自己 `malloc`,那任务光栈就占的那块内存是谁分配的?这一章就是把这件事讲透:FreeRTOS 的动态内存从哪来、谁在管、怎么观测它。

结论一句话:**堆是任务的家**。任务(以及队列、信号量、定时器、事件组这些内核对象)的内存默认全都是从 FreeRTOS 自己管的一块堆里 `pvPortMalloc` 出来的,`xTaskCreate` 内部就是去堆里 malloc 栈和 TCB。**所以得先把堆准备好,才能创建任务**——这正是本章排在任务(下一章)前面的原因。

## TL;DR:五种堆怎么选 + 怎么观测

| 用途 | 选哪个 | 备注 |
|------|--------|------|
| 只分配不释放的极简系统 | **heap_1** | 只有 `pvPortMalloc`、确定性最强、无碎片 |
| 需要动态分配/释放的现代项目 | **heap_4** | 合并相邻空闲块、有最低水位探针,新项目首选 |
| 宿主机/有成熟 `malloc` | **heap_3** | 薄包装标准库,**不实现** `xPortGetFreeHeapSize` |
| RAM 分段(多块不连续内存) | **heap_5** | 用前要先 `vPortDefineHeapRegions()` |
| (历史遗留) | heap_2 | 不合并碎片,新项目别选 |

观测三件套:`configTOTAL_HEAP_SIZE`(堆大小旋钮)、`xPortGetFreeHeapSize()`(此刻余量)、`xPortGetMinimumEverFreeHeapSize()`(历史最低水位,只有 heap_4/5 实现,判断堆够不够的真正依据)。`vApplicationMallocFailedHook` 则是堆分不出来的硬刹车,看到 `ASSERT FAILED` 先怀疑堆太小。

⚠️ host 模拟体量错觉:POSIX port 下一个任务栈有 128KB(`configMINIMAL_STACK_SIZE` × 8B),所以 demo 和 dashboard 都把 `configTOTAL_HEAP_SIZE` 抬到 **2MB**,这和真 MCU 上几百字节栈完全是两个世界。

## 各文章

- [五种堆实现与观测探针](./01-heap-implementations.md) —— 概念/原理篇:内存从哪来、为什么内存在任务之前、heap_1~heap_5 逐个对比与选型、`configTOTAL_HEAP_SIZE` 旋钮与两把探针、`malloc failed hook`
- [独立 demo:分配、释放、观测](./02-standalone-demo.md) —— 独立 demo 篇:`code/03_memory` 切到 heap_4、堆抬 2MB、探针包成一行打印、分配/释放循环,逐行读懂 `free` 与 `min_ever` 的输出
- [dashboard 增量:内存余量监控](./03-dashboard-monitor.md) —— dashboard 增量篇:往脊柱工程加一个常驻轻量监控任务当长期水位计,附本章小结

## 关于踩坑

本章两处 host 模拟的体量/行为坑(POSIX port 下任务栈 128KB 导致要把堆抬到 MB 级、heap_3 在宿主机上几乎无限分配而无法观测),以及更系统性的「host 模拟 vs 真 MCU 内存行为差异」,收在 [仿真坑点](../../pitfalls/) 里,值得对照着读。下一章 [任务管理](../04_tasks/) 我们正式进入 `xTaskCreate`——既然知道任务的栈和 TCB 从堆里来,就可以动手创建任务、排优先级、观察调度器怎么切换了。
