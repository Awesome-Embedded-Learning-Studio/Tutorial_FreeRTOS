---
title: dashboard 增量:模拟按钮中断,经信号量触发 sensor 立即采样
description: 中断章第三页——把中断↔任务同步嫁接到 dashboard 脊柱:给 sensor 任务加一个「按钮」唤醒源,用一个 2500ms 周期软件定时器扮演「按钮 ISR」,经一把二值信号量触发 sensor 立即额外采一帧。讲清两个 host 模拟特有的稳妥取舍(故意不调 portYIELD_FROM_ISR、主信号量短超时 + 旁路信号量零超时替代队列集),以及本章小结
---

# dashboard 增量:模拟按钮中断,经信号量触发 sensor 立即采样

上一页我们在独立 demo 里把延迟中断处理演了一遍,这一页我们把这个能力嫁接到贯穿全教程的 dashboard 脊柱上。dashboard 此前(上一章)sensor 采集任务的唤醒源只有一个——一个 800ms 的周期软件定时器,到期时给 sensor 任务发个任务通知,代表「该定时采样了」。本章增量是**模拟一个「按钮中断」**:除了那个周期滴答,再给 sensor 任务加一个「按钮」唤醒源——按钮「按下」时(我们用一个 2500ms 周期的软件定时器模拟),经一把二值信号量告诉 sensor「按钮中断了,请立刻额外采一帧」。这样输出里「周期采样」(每 800ms)和「按钮中断采样」(每 2500ms)交错出现,一眼能分清,正好把「用信号量把中断事件从类 ISR 上下文递交到任务」演在了一个真实的多任务应用里。

扮演「按钮 ISR」的回调和独立 demo 那个范本同构——给信号量、立刻返回,只不过这把信号量是专属于按钮的:

```c
/* 「按钮源」回调:跑在定时器服务任务上下文,扮演「按钮 ISR」。
 * 它「发生」时给一把二值信号量,把「按钮按下了」这件事递交到 sensor 任务。 */
static void prvButtonCallback( TimerHandle_t xTimer )
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    ( void ) xTimer;
    xSemaphoreGiveFromISR( xButtonSemaphore, &xHigherPriorityTaskWoken );
    ( void ) xHigherPriorityTaskWoken;
}
```

这里有个 host 模拟特有的稳妥取舍,值得展开一句。理论上 give 完应该照独立 demo 那样调 `portYIELD_FROM_ISR(woken)`,但我们在 dashboard 里**故意没调**。原因是:实测在 POSIX port 下,「定时器服务任务上下文里 `xSemaphoreGiveFromISR` + 紧接着 `portYIELD_FROM_ISR`」这个组合,在多任务场景里偶发会让定时器服务任务卡住(stall),demo 就不动了。去掉 `portYIELD_FROM_ISR` 后,sensor 任务(高优先级)会在下一个正常的调度点(tick、阻塞超时)被切上,虽然不如「中断退出即切换」那么即时,但在 host 模拟下完全够用,而且稳得多。注意这只是 host 模拟的稳妥取舍,**真硬件 ISR 里 give 后调 `portYIELD_FROM_ISR` 既是标准、也是正确的写法**,别把这个取舍误读成「真硬件也该省掉 yield」。

另一个要交代的设计点是:sensor 任务现在有**两个**唤醒源(周期滴答 + 按钮),按 FreeRTOS 的标准做法,「一个任务同时等多个信号量/队列」应该用**队列集(Queue Set)**——把两把信号量都加进一个队列集,任务在队列集上阻塞,醒来后查「是哪个成员叫醒了我」。但我们在 dashboard 里**没有用队列集**,而是用了更朴素的写法:主阻塞在周期滴答信号量上(带 50ms 短超时),每一轮顺手用零超时探一下按钮信号量,有就额外采一帧。原因还是实测——「定时器服务任务里 FromISR + 队列集等待」这个组合在 POSIX port 下会稳定 stall,我们试过队列集版本,定时器跑两下就卡死。「主信号量短超时 + 旁路信号量零超时」这套写法既绕开了这个 stall、又能区分两个来源,稳如老狗。sensor 任务长这样:

```c
for( ; ; )
{
    /* 主阻塞:等周期滴答信号量,带 50ms 短超时。拿到就采一帧「周期帧」。 */
    if( xSemaphoreTake( xSampleSemaphore, pdMS_TO_TICKS( SAMPLE_POLL_TIMEOUT_MS ) ) == pdPASS )
    {
        prvTakeAndEnqueueSample( &ulSeq, &lValue, false );   /* 来源:periodic */
    }

    /* 旁路:零超时探一下按钮信号量,有就额外采一帧标 [BUTTON!]。
     * 主阻塞每 50ms 醒一次,按钮最长延迟不超过这个窗口,够及时、又不靠队列集。 */
    if( xSemaphoreTake( xButtonSemaphore, 0 ) == pdPASS )
    {
        prvTakeAndEnqueueSample( &ulSeq, &lValue, true );    /* 来源:button */
    }
}
```

构建运行:

```bash
cd code/dashboard
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 5 ./build/dashboard
```

输出长这样,周期采样(800ms)和按钮采样(2500ms)交错,按钮那一帧带显眼的 `[BUTTON!]` 标记:

```
dashboard: starting scheduler (heap=2097152 bytes, sample=800 ms, button=2500 ms)
heap monitor: free=1702496  min_ever=1702496
sensor: sample #0 value=0 (periodic) sent to queue
display: showing sample #0 value=0 (sampled 0 ms ago, prev #0)
sensor: sample #1 value=1 (periodic) sent to queue
display: showing sample #1 value=1 (sampled 0 ms ago, prev #0)
heap monitor: free=1702496  min_ever=1702496
sensor: sample #2 value=2 (periodic) sent to queue
display: showing sample #2 value=2 (sampled 0 ms ago, prev #1)
sensor: sample #3 value=3 (BUTTON! extra sample) sent to queue
display: showing sample #3 value=3 (sampled 0 ms ago, prev #2)
sensor: sample #4 value=4 (periodic) sent to queue
display: showing sample #4 value=4 (sampled 0 ms ago, prev #3)
sensor: sample #5 value=5 (periodic) sent to queue
display: showing sample #5 value=5 (sampled 0 ms ago, prev #4)
heap monitor: free=1702496  min_ever=1702496
```

仔细读这条输出,你会发现 sample #3 那一帧是 `[BUTTON! extra sample]`——它出现的时刻大约在第 2.5 秒(按钮源第一次触发),恰好「插」在两个周期采样之间,这就是「按钮中断触发了一次额外采样」的直观证据。再往下到第 5 秒附近,你会看到下一帧 `BUTTON!`(sample #7)。它和前后的周期采样明显是两路来源,这就是我们想要演示的「周期滴答」与「按钮中断」并存、各走各的信号量、sensor 任务都能响应的样子。

留意 `heap monitor: free=1702496` 这个数。上一章(06)加了周期采样定时器后是 `1702968`,本章又加了一把按钮信号量 + 一个按钮定时器,掉到了 `1702496`——差了约 470 字节,正好是一个信号量对象 + 一个定时器对象(结构体 + 名字串)的量级。每次加原语,free heap 都会相应下降一点,这是渐进脊柱的「账本」,到第 14 章集成时你会看到完整的一条下降曲线。另外注意每帧的 `sampled 0 ms ago`——从 sensor 采样到 display 消费的延迟几乎为 0,说明即便多了按钮这一路、即便我们没用队列集、即便去掉了 `portYIELD_FROM_ISR`,采集→队列→显示的链路依然运转顺滑,没有被这套中断模拟拖出可感的延迟。

## 小结

走到这里,中断管理的全貌就拼起来了。**ISR 的三条硬规矩**是它区别于任务上下文的全部本质:必须极快返回(把长活丢给任务做,这就是延迟中断处理)、绝不能阻塞(没有任务状态可挂、没有优先级可继承)、只能用 `*FromISR` 系列 API(任务版函数的上下文检查和自作主张的切换对 ISR 既不安全又无意义)。**二值信号量**是 ISR 告诉任务「事件来了」最经典的工具——ISR 在类中断上下文里 `xSemaphoreGiveFromISR` 给一个 token、处理任务在 `xSemaphoreTake` 上阻塞死等被唤醒;计数信号量是它的「可装多个 token」版本,用于统计事件积压或管理资源池。**延迟中断处理**这套模式的精髓是分工:ISR 只 give 一下就返回、把中断延迟压到最低,真正的活全在处理任务的普通任务上下文里干,那里没有任何 ISR 限制。**portYIELD_FROM_ISR** 用来在中断退出那一刻按需切到被唤醒的高优先级任务,配合 FromISR 函数的 `xHigherPriorityTaskWoken` 出参使用,三步走(woken 初始化、give、按需 yield)是标准骨架。

而 host 模拟这一章最该带走的边界认知是:**POSIX port 没有真中断,而且从外部 pthread 直调 `*FromISR` 会 hang**——port 的中断嵌套计数和信号调度只认它自己跟踪的上下文。安全模拟中断源的办法,是让「触发」发生在**软件定时器回调**或 **tick hook** 这两个 port 正确跟踪的上下文里,它们扮演真硬件 ISR 的角色;但这不是真中断、不实时,我们演示的是可移植的模式而非真实的延迟数字。这套模式搬到真硬件,把「定时器回调里的 give」换成「真 ISR 里的 give」即可,一行不用改。关于「ISR API hang」「队列集 stall」这些 host 特有的坑点细节,收在 [仿真坑点](../../pitfalls/) 里,值得对照本章读一遍。下一章 [资源管理](../08_resources/) 我们离开中断、进入另一类同步问题:多个任务争抢同一份共享资源时,互斥量怎么出场、优先级反转为什么会发生、优先级继承又怎么救场——你会发现,二值信号量和互斥量长得几乎一样,但有一个关键差别(优先级继承),正是这个差别决定了「保护共享资源该用谁」。
