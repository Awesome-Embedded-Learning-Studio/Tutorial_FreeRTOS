---
title: 独立 demo:多优先级任务 + 状态快照
description: 04 章 demo 篇——仓库 code/04_tasks/ 的可运行 demo,三个不同优先级的 Worker 先忙等后 vTaskDelay、一个最低优先级的 Monitor 用 uxTaskGetSystemState 拍状态快照,把抢占式优先级调度和 ready/running/blocked 状态机在真实输出里看清楚
---

# 独立 demo:多优先级任务 + 状态快照

> 上一篇 [概念与机制](./01-concepts.md) 讲清了 xTaskCreate、优先级、状态机;这一篇把它们串成一个能跑的 demo。下一篇 [dashboard 增量](./03-dashboard.md) 把同样的能力嫁接到脊柱工程上。

## 这个 demo 在干什么

概念讲完,我们写一个能跑的 demo 把上面这些串起来。demo 在仓库的 `code/04_tasks/`,它做的事是这样的:创建三个 Worker 任务(High/Mid/Low,优先级依次为 4/3/2),每个 Worker 先忙等一段 tick 模拟「干活」(占用 CPU),再 `vTaskDelay` 一段不同长度的时间进入 blocked(让出 CPU),循环往复;再创建一个优先级最低的 Monitor 任务,周期性地用 `uxTaskGetSystemState` 把所有任务的状态拍一张快照打印出来。

先看一个配置上的取舍。你可能在别处见过用 `vTaskList` 打印任务状态表,它很方便——但模板的 `FreeRTOSConfig.h` 里 `configUSE_STATS_FORMATTING_FUNCTIONS=0` 把它关了。我们这里不想为了 demo 去改这个配置开关(样板尽量不动),于是改用 `uxTaskGetSystemState`:它只要 `configUSE_TRACE_FACILITY=1`(模板已开)就可用,返回的是一个 `TaskStatus_t` 结构体数组,我们自己格式化打印,反而比 `vTaskList` 的固定格式更可控。这是「vTaskList 或打印」里的「打印」路线,顺带认识一个更底层的 API。

## Worker:干完活主动让出

Worker 共用一份任务代码,靠 `pvParameters` 区分各自配置。它的核心是「干完活主动 `vTaskDelay` 进 blocked」,我们看这段:

```c
/* Worker 任务:每轮先忙等 active 时长(占用 CPU),再 vTaskDelay sleep 时长
 * (让出 CPU、进入 blocked),循环往复。 */
static void prvWorkerTask( void *pvParameters )
{
    WorkerConfig_t *pxCfg = ( WorkerConfig_t * ) pvParameters;

    for( ; ; )
    {
        console_print( "[%s] running, working %lu ticks...\n",
                       pxCfg->pcName,
                       ( unsigned long ) pxCfg->xActiveTicks );

        prvBurnCycles( pxCfg->xActiveTicks );

        console_print( "[%s] work done, sleeping %lu ticks (blocked)\n",
                       pxCfg->pcName,
                       ( unsigned long ) pxCfg->xSleepTicks );

        /* vTaskDelay 把自己挂到 blocked 直到超时;这段时间 CPU 让给别人。 */
        vTaskDelay( pxCfg->xSleepTicks );
    }
}
```

这里 `prvBurnCycles` 是个故意忙等的函数(一个 `while` 循环空转到指定 tick 数),用来逼真模拟「任务在占用 CPU 干活」。三个 Worker 的 `xSleepTicks` 设成 400/700/1000,周期不同,于是它们进入 blocked 的时刻彼此错开,调度器才有机会在不同时刻挑出不同优先级的 ready 任务来跑——如果三个同步进 blocked、同步醒来,调度轨迹就单调乏味了。每次进入 running 和进入 blocked 都打一行,把调度器的决策链拼出来。

## Monitor:用 uxTaskGetSystemState 拍快照

Monitor 任务用 `uxTaskGetSystemState` 拍快照。它一次性把系统里所有任务(含 idle 和 timer 服务任务)的当前状态填进一个数组,返回填了多少条:

```c
UBaseType_t uxCount = uxTaskGetSystemState( pxStatus, uxMaxTasks, NULL );

/* 简单选择排序:按优先级降序。任务数很少(几个),O(n^2) 完全无所谓。 */
for ( UBaseType_t i = 0; i + 1 < uxCount; i++ )
{
    for ( UBaseType_t j = i + 1; j < uxCount; j++ )
    {
        if ( pxStatus[ j ].uxCurrentPriority > pxStatus[ i ].uxCurrentPriority )
        {
            TaskStatus_t xTmp = pxStatus[ i ];
            pxStatus[ i ] = pxStatus[ j ];
            pxStatus[ j ] = xTmp;
        }
    }
}
```

我们拿到数组后按优先级降序排一下再打印,让高优先级在上,和「调度器优先选高优先级」的直觉一致。每条快照打出任务名、当前状态、优先级、剩余栈高水位。Monitor 自己的优先级压到最低(只比 idle 高一档),确保它只在「没有正经任务想跑」时才插进来——否则它自己就会和 Worker 抢 CPU,拍的快照就被自己污染了。构建和运行还是老配方:

```bash
cd code/04_tasks
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/04_tasks
```

(命令里那两个开关的来历、`stdbuf -oL` 为什么不能少、`timeout 4` 怎么回事,全在 [02 环境搭建](../02_environment/) 里讲过,这里不重复。)运行起来你会看到这样的输出:

```
04_tasks: starting scheduler (configMAX_PRIORITIES=7)
[WorkerHigh] running, working 50 ticks...
[WorkerHigh] work done, sleeping 400 ticks (blocked)
[WorkerMid] running, working 40 ticks...
[WorkerMid] work done, sleeping 700 ticks (blocked)
[WorkerLow] running, working 30 ticks...
[WorkerLow] work done, sleeping 1000 ticks (blocked)
[WorkerHigh] running, working 50 ticks...
[WorkerHigh] work done, sleeping 400 ticks (blocked)
---- task snapshot (prio desc) ----
  Tmr Svc      state=Blocked   prio=6 freestack=32763
  WorkerHigh   state=Blocked   prio=4 freestack=16379
  WorkerMid    state=Blocked   prio=3 freestack=16379
  WorkerLow    state=Blocked   prio=2 freestack=16379
  Monitor      state=Running   prio=1 freestack=16379
  IDLE         state=Ready     prio=0 freestack=16379
-----------------------------------
```

## 读输出:把调度器和状态机看出来

这份输出值得逐行读。先看 Worker 的运行轨迹:`WorkerHigh` 一马当先,因为开局只有它 ready、优先级最高;它干完活进 blocked 后,`WorkerMid` 才轮到,然后是 `WorkerLow`。当三者都在 blocked、且 Monitor 也还没到点时,系统就只剩 idle 任务可跑——这正是优先级调度的样子:**谁 ready 且优先级最高,谁上;大家都 blocked 时,idle 兜底**。`WorkerHigh` 的 `sleep=400` 最短,所以它最先醒来、频率最高,在输出里出现得最密集,这也直观印证了「周期短的高优先级任务会频繁打断调度」。

再看那一张快照,信息量很大。`Tmr Svc` 是定时器服务任务(因为我们开了 `configUSE_TIMERS`,内核自动建了它),优先级 6(模板设成 `configMAX_PRIORITIES-1`),此刻 Blocked——它平时阻塞在自己的命令队列上,有定时器到期才被唤醒。三个 Worker 此刻全在 Blocked(都被各自的 `vTaskDelay` 挂着),`Monitor` 是 Running(拍快照这一刻正是它在跑),`IDLE` 是 Ready(随时能兜底)。这一行 `Monitor=Running / IDLE=Ready / Worker=Blocked` 组合,是「此刻没有任何 Worker 想干活」的铁证——如果此刻有个 Worker 处于 Ready,它一定会压过 Monitor 先跑,你就看不到 Monitor 处于 Running 了。这就是把状态机从代码里「拍」出来给你看的效果。

那个 `freestack` 是 `usStackHighWaterMark`,单位同样是**字**(StackType_t),不是字节——所以 `16379` 不是 16379 字节,真要换算成字节得乘上 `sizeof(StackType_t)`(64 位上是 8)。它表示「这个任务的栈从启动到现在,最多用到离栈底还剩多少字」,是排查「栈够不够」的关键水位:这个数要是贴近 0,说明任务栈快爆了,该把 `usStackDepth` 调大。本章 demo 各任务的 `freestack` 都还很大(16379 字),说明 `configMINIMAL_STACK_SIZE` 给的 16384 字绰绰有余,完全没逼近危险线——这也是 host 模拟下栈「虚胖」的直接体现。

---

下一篇 [dashboard 增量:拆成 sensor + display](./03-dashboard.md) 把这套多任务骨架立到脊柱工程里。
