---
title: dashboard 增量:用周期定时器驱动 sensor 采样
description: 把定时的职责外包给一个 800ms 周期软件定时器——回调只给 sensor 任务发个任务通知就返回,sensor 任务改成「被叫醒再干活」,采样节奏的权威来源从任务内部的 vTaskDelay 挪到独立的软件定时器上;末尾收本章小结
---

# dashboard 增量:用周期定时器驱动 sensor 采样

学完[独立 demo](./02-standalone-demo.md),我们把这个能力嫁接到贯穿全教程的 dashboard 脊柱上。dashboard 此前(05 章)sensor 采集任务的节奏是自己定的——任务里一个 `vTaskDelay(pdMS_TO_TICKS(800))` 睡 800ms 醒来采一帧。本章增量是**把这个定时的职责外包给一个独立的周期软件定时器**:新建一个 800ms 的周期定时器,到期时它的回调给 sensor 任务发一个「该采样了」的轻量通知,sensor 任务收到通知就被唤醒、采一帧、投队列,然后回到阻塞等下一次。采样节奏的权威来源,从「sensor 任务内部的 vTaskDelay」挪到了「独立的软件定时器」上。

这个改造恰好把前面讲的回调铁律演了一遍。定时器回调跑在服务任务上下文,它绝不能在里面干「读传感器、组帧、投队列」这一整套活——虽然这套活在 demo 里其实很短,但正确的姿势永远是「回调只通知、任务才干活」。所以回调只做一件事:给 sensor 任务发个通知就立刻返回:

```c
/* 周期采样定时器回调:跑在 timer 服务任务上下文,只发个通知就返回。
 * 真正的采样(组帧、投队列)交给 sensor 任务去做——回调里绝不干长活。 */
static void prvSampleTimerCallback( TimerHandle_t xTimer )
{
    ( void ) xTimer;
    xTaskNotifyGive( xSensorTaskHandle );   /* 「该采样了」,轻量叫醒铃 */
}
```

(这里用的是**任务通知** `xTaskNotifyGive`/`ulTaskNotifyTake`,而不是信号量,因为它最轻量——没有独立对象、不占队列。「任务通知」这门原语会在第 10 章正式讲透,本章先把它当一个轻量的叫醒铃用,不展开它的全貌。)sensor 任务因此改头换面,它不再靠 `vTaskDelay` 自己定时,而是在 `ulTaskNotifyTake` 上死等通知:

```c
/* [05] 里这里曾是 vTaskDelay(pdMS_TO_TICKS(800));[06] 把定时的职责
 * 外包给了周期软件定时器,sensor 任务改成「被叫醒再干活」。 */
for( ; ; )
{
    ulTaskNotifyTake( pdTRUE, portMAX_DELAY );   /* 阻塞等通知,死等 */

    /* 被唤醒意味着「定时器到期了、该采样」。组帧、投队列…… */
    xFrame.ulSeq   = ulSeq;
    xFrame.lValue  = lValue;
    xFrame.ulTick  = xTaskGetTickCount();
    xQueueOverwrite( xSampleQueue, &xFrame );
    /* ……打一行「采到了」,自增计数器,然后回到阻塞等下一次。 */
}
```

平时 sensor 任务 blocked、不占 CPU;定时器回调一发通知(每 800ms 一次),它就被唤醒、采一帧、投队列,然后回到阻塞。`main()` 里新增的也就是「建定时器 + 启动它」这几行,和独立 demo 如出一辙:

```c
xSampleTimer = xTimerCreate( "SampleTmr",
                             pdMS_TO_TICKS( SAMPLE_TIMER_PERIOD_MS ),
                             pdTRUE, NULL, prvSampleTimerCallback );
configASSERT( xSampleTimer != NULL );
BaseType_t xTimerOk = xTimerStart( xSampleTimer, portMAX_DELAY );
configASSERT( xTimerOk == pdPASS );
```

构建运行:

```bash
cd code/dashboard
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/dashboard
```

输出长这样,采集节奏依然是 800ms 一帧(和 05 章一致——我们没改节奏,只改了节奏由谁掌管),display 照常消费队列、内存监控照常探堆:

```
dashboard: starting scheduler (heap=2097152 bytes, sample timer=800 ms)
heap monitor: free=1702968  min_ever=1702968
sensor: sample #0 value=0 sent to queue
display: showing sample #0 value=0 (sampled 0 ms ago, prev #0)
sensor: sample #1 value=1 sent to queue
display: showing sample #1 value=1 (sampled 0 ms ago, prev #0)
heap monitor: free=1702968  min_ever=1702968
sensor: sample #2 value=2 sent to queue
display: showing sample #2 value=2 (sampled 0 ms ago, prev #1)
sensor: sample #3 value=3 sent to queue
display: showing sample #3 value=3 (sampled 0 ms ago, prev #2)
```

注意看 `heap monitor: free=1702968`。上一章(05)这个数是 `1703272`,本章加了定时器后掉到了 `1702968`——差了约 300 字节,正好是一个定时器对象(它的结构体 + 名字串)的量级,远小于一个任务栈(128KB)。这就是「软件定时器很省」的直接观测:加一个定时器比加一个任务便宜得多,这也是为什么「能定时解决的就别开任务」是个常见优化思路。注意 sensor 任务的 `sampled 0 ms ago` 显示——从采样到 display 消费的延迟几乎是 0,说明定时器→通知→任务的链路在 host 模拟下运转得很顺,没有可感的拖延迟。

有个细节值得品:本章我们没改 sensor 的采样节奏(还是 800ms),只是把「谁来定这个 800ms」从任务内部的 `vTaskDelay` 换成了独立的软件定时器。表面看行为一样,但架构上不一样了——采样节奏现在是一个可被统一管理的对象:你以后想动态改采样频率,调 `xTimerChangePeriod` 一行就行,不用去翻任务代码改 `vTaskDelay` 的参数;想让采样「暂停再恢复」,`xTimerStop`/`xTimerStart` 即可;想让外部事件(比如一个按钮)立刻触发一次额外采样,也只需在那个事件里给 sensor 任务补发一个通知。节奏外置成定时器,灵活度一下就上来了,这正是软件定时器相对于「任务里 vTaskDelay」的核心价值。

## 小结

走到这里,软件定时器的全貌就拼起来了:**`xTimerCreate` 的五个参数**分别是名字(调试用)、周期(tick,周期定时器是间隔、单次定时器是首次延迟)、`uxAutoReload`(pdTRUE 周期 / pdFALSE 单次,这一位是两种模式的全部区别)、pvTimerID(多定时器共用回调时区分身份)、回调函数;**单次 vs 周期**就差 `uxAutoReload` 一位,周期周而复始、单次响一次就 dormant,对应「持续节奏」和「过段时间做一次」两类需求;**回调跑在 timer 服务任务上下文**,所有回调串行挤在这一个任务里,所以回调必须极短、绝不阻塞(vTaskDelay/死等/长活全禁止),正确姿势是「回调只通知、任务才干活」,否则会拖垮全体定时器;**定时器命令队列**让 `xTimerStart` 这类 API 不立刻生效——它投命令、服务任务取命令才真正执行,所以软件定时器精度受服务任务调度及时性和队列长度限制,不是硬件中断、host 模拟下不实时;**配置旋钮**里 `configUSE_TIMERS` 是总开关,`configTIMER_TASK_PRIORITY` 默认最高档(让回调少迟到),`configTIMER_TASK_STACK_DEPTH` 给回调留栈(所有回调共用这一个栈,回调重了得往上抬)。

至于「软件定时器和真 MCU 在行为上的差异」——精度受 host 调度抖动影响、回调上下文在模拟和真硬件上一致(都跑服务任务)、以及「别在回调里干重活」这条在两边都成立——属于系统性的仿真坑点,收在 [仿真坑点](../../pitfalls/) 里,值得对照本章读一遍。回到[章节总览](./index.md)看本章脉络,下一章 [中断管理](../07_interrupts/) 我们进入 RTOS 里 host 模拟最棘手的一块:ISR 上下文、`*FromISR` API、用信号量做中断和任务的同步,并且会专门讲清「POSIX port 下不能从外部 pthread 直调 `*FromISR`」这个大坑——到时候你会发现,软件定时器回调(以及 tick hook)正是 host 模拟下「相对安全地模拟中断源」的常用落脚点,本章讲的「回调上下文」会直接派上用场。
