---
title: 独立 demo:一个周期定时器 + 一个单次定时器
description: 跟着仓库 code/06_timers/ 的 demo 把软件定时器机制演给你看——一个每 500ms 响一次的周期定时器、一个 2000ms 后响唯一一次的单次定时器,回调极短绝不阻塞,运行输出把「周期周而复始、单次响一次就 dormant」演得明明白白
---

# 独立 demo:一个周期定时器 + 一个单次定时器

概念上一页[软件定时器:概念与机制](./01-concepts.md)讲完了,这一页配上能跑的 demo 把上面这些串起来。demo 在仓库的 `code/06_timers/`,它做的事很直接:创建两个定时器——一个周期定时器(每 500ms 响一次)、一个单次定时器(2000ms 后响唯一一次),两个回调都打印触发时刻(把当前 tick 当毫秒看),并在前面标上自己的名字。这样输出里你能一眼分清「这条是周期定时器每半秒响一次」「那条是单次定时器第 2000ms 响了那一下、之后再也没响」。

先看两个定时器是怎么造出来的,这是 `xTimerCreate` 的完整用法:

```c
/* 周期定时器:uxAutoReload=pdTRUE,到期后自动重装、周而复始。 */
TimerHandle_t xPeriodicTimer = xTimerCreate(
    "Periodic",
    pdMS_TO_TICKS( PERIODIC_TIMER_PERIOD_MS ),   /* 500ms 周期 */
    pdTRUE,                                       /* pdTRUE -> 周期 */
    NULL,
    prvPeriodicTimerCallback );

/* 单次定时器:uxAutoReload=pdFALSE,到期一次就停、退休。 */
TimerHandle_t xOneShotTimer = xTimerCreate(
    "OneShot",
    pdMS_TO_TICKS( ONESHOT_TIMER_PERIOD_MS ),    /* 2000ms 后响一次 */
    pdFALSE,                                      /* pdFALSE -> 单次 */
    NULL,
    prvOneShotTimerCallback );
```

两个回调的写法是本章的范本——极短、绝不阻塞、打印完立刻返回:

```c
/* 周期定时器回调:跑在 timer 服务任务上下文,只打印此刻就返回。 */
static void prvPeriodicTimerCallback( TimerHandle_t xTimer )
{
    ( void ) xTimer;
    prvPrintTickPrefix( "periodic timer fired  ->" );
    console_print( " (repeats every %d ms)\n", PERIODIC_TIMER_PERIOD_MS );
}

/* 单次定时器回调:同样只打印、立刻返回,触发完这个定时器就 dormant。 */
static void prvOneShotTimerCallback( TimerHandle_t xTimer )
{
    ( void ) xTimer;
    prvPrintTickPrefix( "one-shot timer fired ->" );
    console_print( " (fires ONCE after %d ms, then retires)\n",
                   ONESHOT_TIMER_PERIOD_MS );
}
```

启动它们用 `xTimerStart`,它把「请启动」这条命令投进定时器命令队列,第二个参数是「命令队列满了等多久」:

```c
BaseType_t xOk1 = xTimerStart( xPeriodicTimer, portMAX_DELAY );
BaseType_t xOk2 = xTimerStart( xOneShotTimer, portMAX_DELAY );
configASSERT( xOk1 == pdPASS );
configASSERT( xOk2 == pdPASS );
```

这里两个 `xTimerStart` 都在 `vTaskStartScheduler` 之前调——这是允许的,内核会在服务任务起来后依次处理这些早投的命令(你会在 `app_hooks.c` 的 `vApplicationDaemonTaskStartupHook` 那个时机附近看到服务任务开始干活)。构建和运行还是老配方:

```bash
cd code/06_timers
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/06_timers
```

(`stdbuf -oL` 为什么不能少、`timeout 4` 怎么回事,全在 [02 环境搭建](../02_environment/) 里讲过,这里不重复。)运行起来你会看到这样的输出:

```
06_timers: periodic=500ms (auto-reload), one-shot=2000ms (single fire)
06_timers: starting scheduler
[  500 ms] periodic timer fired  ->  (repeats every 500 ms)
[ 1000 ms] periodic timer fired  ->  (repeats every 500 ms)
[ 1500 ms] periodic timer fired  ->  (repeats every 500 ms)
[ 2000 ms] one-shot timer fired ->  (fires ONCE after 2000 ms, then retires)
[ 2000 ms] periodic timer fired  ->  (repeats every 500 ms)
[ 2500 ms] periodic timer fired  ->  (repeats every 500 ms)
[ 3000 ms] periodic timer fired  ->  (repeats every 500 ms)
```

这份输出值得逐行读,因为它把单次和周期的差别演得明明白白。看周期定时器:它从 500ms 开始,每隔 500ms 准时响一次——500、1000、1500、2000、2500、3000,一条等间隔的线,只要进程不退出它就这么滚下去。再看单次定时器:它在第 2000ms 响了唯一一次,然后……就再也没有它的输出线了。你再往下翻,3000ms、4000ms 全是周期定时器在那儿响,单次定时器彻底沉默。这就是「打完一枪就退休」的样子——单次定时器触发一次后进入 dormant,除非你重新 `xTimerStart`,否则它再也不响。

注意第 2000ms 那一刻,周期和单次两个回调都响了,而且都准确地落在 2000ms。这不是巧合,而是 host 模拟下 tick 精度足够高(1 tick=1ms)、且两个回调都很短(没把服务任务卡住)的结果。你可以做个思想实验:如果我把周期定时器的回调改成「打印完 `vTaskDelay(300)` 假装在干活」(这就是前面说的「回调里阻塞」的典型反面教材),你会看到什么?第 2000ms 处,服务任务先跑周期回调,这一卡就是 300ms,单次定时器的回调虽然 2000ms 就到期了,却得等服务任务空出来才能跑——于是它会迟到 300ms 左右,周期定时器后续的触发也全会被推后。这正是「回调里阻塞会拖垮全体定时器」的现场。这个反面教材我们不在 demo 里真跑(跑出来太破坏观感),但你心里要有这个画面,以后写回调时会自觉保持它极短。

下一页[dashboard 增量:用周期定时器驱动 sensor 采样](./03-dashboard-increment.md)把这个能力嫁接到贯穿全教程的 dashboard 脊柱上。
