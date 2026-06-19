---
title: 独立 demo:用定时器回调模拟中断源,经二值信号量唤醒处理任务
description: 中断章第二页——跟着仓库 code/07_interrupts/ 的独立 demo 把延迟中断处理整套演出来:周期软件定时器回调扮演「中断源」,在定时器服务任务上下文里 xSemaphoreGiveFromISR 给一把二值信号量立刻返回(ISR 姿势),处理任务在 xSemaphoreTake 上阻塞死等被唤醒干延迟的活,完整走一遍三条规矩、give/take、portYIELD_FROM_ISR 三步骨架
---

# 独立 demo:用定时器回调模拟中断源,经二值信号量唤醒处理任务

上一页我们把 ISR 的三条规矩、二值信号量、portYIELD_FROM_ISR 都讲透了,这一页配上能跑的 demo 把延迟中断处理整套演出来。demo 在仓库的 `code/07_interrupts/` 里,它做的事很直接:用一个周期软件定时器扮演「中断源」(每 500ms 「发生一次」),它的回调在定时器服务任务上下文里给一把二值信号量、立刻返回(这是 ISR 的姿势);另一边一个处理任务在信号量上阻塞死等,被唤醒就去「干延迟了的活」(打一行、报个时间戳),然后回到阻塞等下一次。我们借此把上一页讲的三条规矩、give/take、portYIELD_FROM_ISR 全部走一遍。

先看扮演 ISR 的那个回调,这是本章的范本——它严格按 ISR 的三条规矩写:

```c
/* 模拟中断源的「类 ISR」回调:跑在定时器服务任务上下文,扮演真硬件 ISR。
 * 三条 ISR 规矩一条不落:极短返回、绝不阻塞、只用 FromISR API。 */
static void prvSimulatedIsr( TimerHandle_t xTimer )
{
    static BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    ( void ) xTimer;

    /* 第一步:woken 出参初始化成 pdFALSE(FromISR 的固定写法)。 */
    xHigherPriorityTaskWoken = pdFALSE;

    /* 第二步:给信号量——这就是「中断发生了」这一下。处理任务若正阻塞在
     * xSemaphoreTake 上,这一 give 就把它唤醒。注意用的是 FromISR 版:
     * ISR 上下文只能用带 FromISR 后缀的 API,用普通的 xSemaphoreGive 是错的。 */
    xSemaphoreGiveFromISR( xIrqSemaphore, &xHigherPriorityTaskWoken );

    /* 第三步:如果刚才那次 give 唤醒了高优先级任务,请求中断退出时切到它。 */
    portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
}
```

这三步就是中断↔任务同步的标准骨架,真硬件上把函数体放进 GPIO ISR、把 `xTimer` 参数去掉,逻辑完全一样。处理任务那一半更简单——它在普通任务上下文里阻塞 take,醒了就干活:

```c
/* 处理任务:在二值信号量上阻塞死等,被唤醒就干延迟了的活。
 * 关键:它在普通任务上下文里,可以放心用任何 API——这正是「延迟」的精髓。 */
static void prvHandlerTask( void *pvParameters )
{
    ( void )pvParameters;
    for( ; ; )
    {
        /* take 用的是任务上下文版(不是 FromISR):take 在任务、give(FromISR)在
         * 类 ISR,这是中断↔任务同步的标配分工。portMAX_DELAY 表示死等。 */
        xSemaphoreTake( xIrqSemaphore, portMAX_DELAY );

        /* 到这里就是「被中断唤醒、开始干活」。打时间戳、格式化打印这些活,
         * 如果放进真 ISR 里是不合适的(ISR 要尽量短、尽量别 printf);正因为我们
         * 把它们推迟到了这个任务里,才毫无顾忌——这就是延迟中断处理的价值现场。 */
        console_print( "[handler] woken by simulated IRQ, doing deferred work at tick %lu\n",
                       ( unsigned long ) xTaskGetTickCount() );
    }
}
```

`main()` 里把信号量建出来、把处理任务建出来、把「中断源」定时器建好启动,套路和上一章一致。注意二值信号量 `xSemaphoreCreateBinary` 创建出来是「空」的(没人 give 过),所以处理任务一启动就会阻塞在 take 上、老老实实等第一次「中断」——这正是我们要的「任务先就位、等中断来叫它」:

```c
/* 创建二值信号量(初始为空)和处理任务,再建好并启动「中断源」定时器。 */
xIrqSemaphore = xSemaphoreCreateBinary();
configASSERT( xIrqSemaphore != NULL );

xTaskCreate( prvHandlerTask, "Handler", configMINIMAL_STACK_SIZE, NULL,
             prioHANDLER, &xHandlerTaskHandle );

xIrqSourceTimer = xTimerCreate( "IrqSrc",
                                pdMS_TO_TICKS( IRQ_SOURCE_PERIOD_MS ),
                                pdTRUE, NULL, prvSimulatedIsr );
configASSERT( xIrqSourceTimer != NULL );
configASSERT( xTimerStart( xIrqSourceTimer, portMAX_DELAY ) == pdPASS );
```

构建和运行还是老配方(`stdbuf -oL`、`timeout` 为什么不能少,全在 [02 环境搭建](../02_environment/) 讲过):

```bash
cd code/07_interrupts
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/07_interrupts
```

运行起来你会看到这样的输出——处理任务每 500ms 被唤醒一次,每次都准确落在「中断源」触发后:

```
07_interrupts: deferred-ISR demo, simulated IRQ every 500 ms
07_interrupts: starting scheduler
[handler] woken by simulated IRQ, doing deferred work at tick 500
[handler] woken by simulated IRQ, doing deferred work at tick 1000
[handler] woken by simulated IRQ, doing deferred work at tick 1500
[handler] woken by simulated IRQ, doing deferred work at tick 2000
[handler] woken by simulated IRQ, doing deferred work at tick 2500
[handler] woken by simulated IRQ, doing deferred work at tick 3000
```

这份输出读起来平淡,但平淡本身就是正确运行的证据——它说明那条「定时器回调 give 信号量 → 处理任务 take 被唤醒 → 干活 → 回到阻塞」的链路在 host 模拟下稳稳地转着,没 hang、没断言。每 500ms 一次、间隔均匀,说明扮演 ISR 的定时器回调每次都极短地 give 完返回了(没把服务任务卡住),处理任务每次都被及时唤醒。如果你把 `prvSimulatedIsr` 里的 `xSemaphoreGiveFromISR` 改成从一个你自己 `pthread_create` 出来的线程里调,你立刻就会收获本节开头说的那个 hang——这就是「为什么必须从 port 跟踪的上下文里调 FromISR」的活体证明,这个反面实验我们不在 demo 里真跑,但你要心里有数。

下一页 [dashboard 增量:模拟按钮中断](./03-dashboard-button-irq.md) 把这套中断↔任务同步嫁接到贯穿全教程的 dashboard 脊柱里,演一个「按钮中断」触发额外采样。
