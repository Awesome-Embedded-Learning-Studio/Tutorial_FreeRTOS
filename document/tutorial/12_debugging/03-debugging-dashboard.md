---
title: dashboard 增量:周期 CPU 占用统计任务
description: 把运行时统计能力嫁接到 dashboard 脊柱——加一个周期性打印 CPU 占用统计的 prvCpuStatsTask,补全状态快照里缺失的「CPU 占用」那一列,带真实运行输出与解读,并收束本章小结、链向下一章排错
---

# dashboard 增量:周期 CPU 占用统计任务

> 调试这一章第三篇(收尾)。[上一篇](./02-debugging-demo.md) 跑通了独立 demo,这一篇把同样的 CPU 占用统计能力嫁接到贯穿全教程的 dashboard 脊柱上,并在末尾收束本章小结。回[章节导航](./index.md)。

学完独立 demo,我们把这个能力嫁接到贯穿全教程的 dashboard 脊柱上。dashboard 到这里已经攒了 Sensor / Display / HeapMon / Stats / Init / Control 一堆应用任务(外加内核的 idle 和 timer daemon),「它们各自吃多少 CPU」是个非常自然的可观测性诉求。本章增量就是**加一个周期性打印 CPU 占用统计的任务** `prvCpuStatsTask`,正好呼应 [04 任务管理](../04_tasks/) 那张「状态快照」——那里拍的是 state/prio/freestack,这里补上「CPU 占用」那一列,把 dashboard 的可观测性又往前推了一格。

这个任务复用独立 demo 里那套 `uxTaskGetSystemState` + 百分比折算的逻辑,优先级压到最低(和 HeapMon/Stats 同档),只在「没有正经任务想跑」时插进来——否则它自己拍快照、格式化打印的那段 CPU 时间会被记进统计、把数据污染了。核心循环长这样:

```c
configRUN_TIME_COUNTER_TYPE ulTotalRunTime = 0;
UBaseType_t uxCount = uxTaskGetSystemState( xStatus, CPU_STATS_MAX_TASKS, &ulTotalRunTime );

/* 按运行时占用降序排,最吃 CPU 的排最上面。 */
for( UBaseType_t i = 0; i + 1 < uxCount; i++ )
    for( UBaseType_t j = i + 1; j < uxCount; j++ )
        if( xStatus[ j ].ulRunTimeCounter > xStatus[ i ].ulRunTimeCounter )
        { /* 交换 */ }

for( UBaseType_t i = 0; i < uxCount; i++ )
{
    uint32_t ulPct = ( uint32_t ) ( ( xStatus[ i ].ulRunTimeCounter * 100UL ) / ulTotalRunTime );
    /* 打印 任务名 / 状态 / 优先级 / 栈高水位 / CPU% */
}
```

构建运行:

```bash
cd code/dashboard
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 5 ./build/dashboard
```

输出长这样(截取后半段,前面 sensor/display/control 的节奏和上一章一致,这里省略):

```
stats: samples=6 (buttons=1)  displays=6
---- cpu stats @ tick 4002 (configGENERATE_RUN_TIME_STATS=1) ----
  IDLE         Ready    prio=0 freestack=16379  CPU=100%
  HeapMon      Ready    prio=1 freestack=16379  CPU=<1%
  Stats        Ready    prio=1 freestack=16379  CPU=<1%
  CpuStats     Running  prio=1 freestack=16379  CPU=<1%
  Sensor       Blocked  prio=3 freestack=16379  CPU=<1%
  Tmr Svc      Blocked  prio=6 freestack=32763  CPU=<1%
  Control      Blocked  prio=2 freestack=16379  CPU=<1%
  Display      Blocked  prio=2 freestack=16379  CPU=<1%
--------------------------------------------------------
```

这张表读起来,有个一眼就该抓住的信号:`IDLE` 占了 100%。这可不是「idle 任务在死循环」,恰恰相反——它是「所有应用任务都设计得很健康」的铁证。dashboard 里 Sensor 等 800ms 信号量、Display 等队列+事件组、HeapMon/Stats/CpuStats 各自 `vTaskDelay` 几秒、Init 早已自删、Control 每 1.6 秒醒一次……这些任务绝大多数时间都老老实实 blocked、把 CPU 让出来,于是空闲时间几乎全落到了 idle 任务头上。这正是「善用 blocked 是多任务并存的前提」(见 [04 任务管理](../04_tasks/))在运行时统计上的具象化:你把任务都写成「没事干就睡」,idle 占比就高;反过来,要是哪天你看到 dashboard 的 idle 占比反常地低、某个应用任务占比反常地高,那就是它在死循环式地抢 CPU——这就是这章工具帮你抓的「starvation / 死循环」类问题。

注意看 `CpuStats` 自己是 `Running`、占比 `<1%`——它优先级最低、只占空闲,所以既拍得到快照、又几乎不污染统计。`Tmr Svc` 的 `freestack=32763` 比别人大一倍,是因为它的栈深是 `configMINIMAL_STACK_SIZE * 2`(定时器服务任务默认配双倍栈,见 `FreeRTOSConfig.h` 的 `configTIMER_TASK_STACK_DEPTH`)。各任务 `freestack` 都还很大,说明没人逼近栈危险线——host 模拟下这个数会一直虚胖,真 MCU 上才是收紧栈深的依据。

## 小结

走到这里,调试这一章的工具箱就齐了。**`configASSERT`/`vAssertCalled`** 是快速失败的第一道防线,你写错 API 用法(越界优先级、NULL 句柄、ISR 里调错 API)时它当场断言、把内核源文件+行号甩给你,模板里这个回调已经替你接好,真硬件上行为一致。**`uxTaskGetSystemState`** 是拍全任务状态快照的底层 API,它返回 `TaskStatus_t` 数组(任务名/状态/优先级/栈高水位/累计运行时),你自己格式化打印,**比 `vTaskList` 那条受 `configUSE_STATS_FORMATTING_FUNCTIONS` 控制的封装路更可控、也更推荐**。**`uxTaskGetStackHighWaterMark`** 单独探一个任务的栈水位,单位是字不是字节,逼近 0 就是危险信号。**`configGENERATE_RUN_TIME_STATS`** 给出各任务的 CPU 占用百分比,POSIX port 已替我们接好运行时统计时钟,开开关就能用,占比 = `ulRunTimeCounter × 100 / 总运行时`,排查 starvation/死循环的利器。**GDB 下 `handle SIGUSR1 SIG34 nostop noprint`** 让那两个调度信号不再刷屏,这是 host 调试的前置动作,呼应 [运行、读输出与排坑](../02_environment/03-posix-run.md)。

而本章最该带走的边界认知是:**栈溢出检测 `configCHECK_FOR_STACK_OVERFLOW` 在 POSIX port 下不生效**——真 MCU 上内核靠给栈涂魔数、切换时检查来被动报警,但 host 模拟下任务栈是 glibc 管的 pthread 栈、内核够不着,那个 `vApplicationStackOverflowHook` 永远不会被调。host 下我们只能靠 `uxTaskGetStackHighWaterMark`「主动探」水位,代替它「被动报警」;真硬件上则是两条都要、互相补充。至于 host 模拟和真 MCU 在调试行为上的全部系统性差异(栈双重性、ISR API hang、`vTaskDelete` 回收延迟等),收在 [仿真坑点](../../pitfalls/) 里,值得对照本章读一遍。下一章 [排错](../13_troubleshooting/) 我们换个角度:不再讲「工具怎么用」,而是讲「常见症状怎么治」——malloc failed、栈溢出、死锁、优先级反转、队列丢消息、starvation 这些高频问题,逐条给出现象→原因→对策的速查,本章的这套工具正是排错时的眼睛。
