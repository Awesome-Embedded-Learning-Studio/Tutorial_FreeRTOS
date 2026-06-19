---
title: 调试
description: 这章讲什么、工具箱 TL;DR、指向各文章的导航、前后章交叉链接——configASSERT、任务状态快照、栈水位探针、运行时统计怎么用,以及栈溢出检测在 POSIX port 下失效这条最重要的边界
---

# 调试

## 这章解决什么问题

前面十一章我们一直在「写」:任务、队列、信号量、定时器、中断……代码写完只是一半,另一半是「它跑起来不对的时候怎么办」。RTOS 比裸机难调,因为现场转瞬即逝——一个任务踩坏另一个任务的栈、一次错误 API 调用让任务再没被调度、一个优先级设错把别人活活饿死,等你挂上调试器现场早没了。这一章要解决的就是**怎么在 RTOS 里把这种转瞬即逝的现场,变成你能看见的东西**:用 FreeRTOS 自带的观测工具箱(configASSERT、任务状态快照、栈水位探针、运行时统计)把「现在系统里到底发生了什么」拍成快照、折算成 CPU 占用。

## TL;DR:工具箱速查

| 工具 | 回答的问题 | 一句话用法 |
|------|-----------|-----------|
| `configASSERT` / `vAssertCalled` | 「我用法错了」(越界优先级、NULL 句柄、ISR 调错 API) | 内核埋点当场断言,打印 `ASSERT FAILED: <文件>:<行号>` 后卡死,模板已接好回调 |
| `uxTaskGetSystemState` | 「现在所有任务什么状态」 | 填一个 `TaskStatus_t` 数组(名/状态/优先级/栈水位/累计运行时),自己格式化打印 |
| `vTaskList` | 同上,封装好的现成格式表 | 受 `configUSE_STATS_FORMATTING_FUNCTIONS` 控制(模板关着),不如自己格式化推荐 |
| `uxTaskGetStackHighWaterMark` | 「某任务栈还剩多少」 | 返回离栈底的余量,单位是**字**不是字节,逼近 0 是危险信号 |
| `configGENERATE_RUN_TIME_STATS` | 「每个任务各占多少 CPU」 | POSIX port 已接好时钟,开开关即用;占比 = `ulRunTimeCounter × 100 / 总运行时` |
| GDB `handle SIGUSR1 SIG34 nostop noprint` | 「为什么单步总断在信号上」 | POSIX port 用这俩信号做 pthread 间调度同步,ignore 掉才能正常调试 |

⚠️ **最重要的边界**:`configCHECK_FOR_STACK_OVERFLOW`(栈溢出被动检测)在 POSIX port 下**不生效**——内核够不着 glibc 管的 pthread 栈去涂魔数,那个 `vApplicationStackOverflowHook` 永远不会被调。host 下只能靠 `uxTaskGetStackHighWaterMark`「主动探」水位代替它「被动报警」,真硬件上才是两条都要。务必和 [仿真坑点](../../pitfalls/) 对照读。

## 各文章

- [调试工具箱:概念与原理](./01-debugging-concepts.md) —— 这章解决什么、configASSERT 怎么当场拦下错用法、uxTaskGetSystemState/vTaskList 怎么拍状态快照、uxTaskGetStackHighWaterMark 怎么探栈水位、运行时统计怎么算 CPU 占用、GDB ignore 信号技巧、以及栈溢出检测在 POSIX port 下为什么失效
- [独立 demo:断言、状态快照、运行时统计三连](./02-debugging-demo.md) —— 跟着仓库 `code/12_debugging/` 跑通:故意触发 configASSERT、拍全任务状态快照、折算 CPU 占用,带真实运行输出逐行解读
- [dashboard 增量:周期 CPU 占用统计任务](./03-debugging-dashboard.md) —— 加一个 `prvCpuStatsTask` 把 CPU 占用统计嫁接到 dashboard 脊柱,补全状态快照缺失的那一列,并收束本章小结

## 关于踩坑

调试这章和 host 模拟的行为边界强相关:栈溢出检测失效(见上)、`uxTaskGetStackHighWaterMark` 水位在 host 下虚胖不真、GDB 调试要 ignore 两个调度信号——这些都收在 [仿真坑点](../../pitfalls/) 里,建议进具体工具前扫一眼建立预期。本章引用的 [02 环境搭建](../02_environment/) 那套 build/run 老配方(`stdbuf -oL`、构建开关)在本章每个 demo 里都照用。

← 上一章 [低功耗](../11_low-power/)| 下一章 [排错](../13_troubleshooting/) →
