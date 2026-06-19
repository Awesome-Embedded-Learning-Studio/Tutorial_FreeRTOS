---
title: dashboard 增量:用 xTaskNotify 携带模式码让 sensor 切换采样模式
description: 把任务通知嫁接到贯穿全教程的 dashboard 脊柱——新增控制源任务 prvControlTask 用 xTaskNotify 的 eSetValueWithOverwrite 给 sensor 任务下发采样模式(normal/BOOST 来回切),sensor 在循环里用零超时 ulTaskNotifyTake 探通知值取新模式码切模式,和已有的两把无值信号量(周期滴答、按钮中断)区分开,演活「带值控制通道」的独有价值,并附本章小结与下一章预告
---

# dashboard 增量:用 xTaskNotify 携带模式码,让 sensor 切换采样模式

> 任务通知这一章的第三篇(也是最后一篇)。上一篇讲了独立 demo,这一篇把那个「带值的控制通道」嫁接到贯穿全教程的 dashboard 脊柱上,sensor 任务靠它切换采样模式。文末是本章小结和下一章预告,链回 [本章导航](./) 看全貌。

学完独立 demo,我们把这个能力嫁接到贯穿全教程的 dashboard 脊柱上。dashboard 此前的 sensor 采集任务有两路唤醒源:一路是 [06] 的周期滴答(每 800ms 采一帧「周期帧」),另一路是 [07] 模拟的「按钮中断」(每 2500ms 触发一次额外采样、标 `[BUTTON!]`)。这两路我们用的都是信号量——[07] 那一章为了叙事一致,把 [06] 原本的 `xTaskNotifyGive` 也改成了 `xSemaphoreGiveFromISR`,所以 sensor 此前的任务通知值一直空着。本章增量正好把它用起来:新增一个**控制源任务** `prvControlTask`,周期性地用 `xTaskNotify` 给 sensor 下发「采样模式」切换指令(在 `MODE_NORMAL` 普通模式和 `MODE_BOOST` 加速模式之间来回切),sensor 在每轮循环里用零超时的 `ulTaskNotifyTake` 探一下自己的通知值,取到新模式码就切模式、后续采到的帧在输出里带上 `mode=BOOST` 或 `mode=normal` 标记。

这套设计是刻意要和前面已有的两种「轻量唤醒」手段区分开的,把任务通知的独有价值演出来——这是这一章在 dashboard 里最关键的设计考量。我们手里现在有三类「戳一下 sensor」的手段,它们的差别必须讲清:[06] 的周期滴答和 [07] 的按钮事件都是**无值的**——前者最早用 `xTaskNotifyGive`(只是给 sensor 拍一下「到点了」,带不了任何数据),[07] 出于叙事一致把它换成了二值信号量(同样只能表达「事件来了」,塞不进「按的是哪个按钮」);本章的模式切换用 `xTaskNotify` 的 `eSetValueWithOverwrite`,把一个模式码写进 sensor 的通知值,**顺通知一路带过去**。这个差别不是炫技——它对应着真实的通信需求:模式切换这种「带数据」的指令,用信号量做不到(信号量 give 一下就是一下),用任务通知一行搞定。

控制源任务用 `eSetValueWithOverwrite` 覆盖式下发模式码,在 normal 和 boost 之间来回切:

```c
/* [10] 控制源任务:周期性地用 xTaskNotify 给 sensor 下发「采样模式」切换指令。
 * 用 eSetValueWithOverwrite 覆盖式写通知值:sensor 还没消费上一条、控制源又下了一条,
 * 新的覆盖旧的,sensor 永远取到「最新模式」——这正是「最新值」语义,适合控制类通信。 */
static void prvControlTask( void *pvParameters )
{
    ( void ) pvParameters;
    uint32_t ulMode = MODE_BOOST;    /* 第一次切的模式,下一轮先变 BOOST */

    for( ; ; )
    {
        vTaskDelay( pdMS_TO_TICKS( CONTROL_PERIOD_MS ) );

        /* normal ↔ boost 来回切。 */
        ulMode = ( ulMode == MODE_BOOST ) ? MODE_NORMAL : MODE_BOOST;

        /* xTaskNotify:给 sensor 任务发通知,把 ulMode 覆盖式写进它的通知值。 */
        xTaskNotify( xSensorTaskHandle, ulMode, eSetValueWithOverwrite );

        console_print( "control: sent notify value=%lu (%s)\n",
                       ( unsigned long ) ulMode,
                       ulMode == MODE_BOOST ? "BOOST" : "normal" );
    }
}
```

sensor 任务那一边,在原来的「主信号量阻塞 + 旁路按钮信号量零超时探一下」循环里,再加一路旁路:零超时探一下自己的任务通知值,有新模式码就切。三路唤醒源就这样并存在同一个循环里,各自走各自的原语、互不抢通道:

```c
for( ; ; )
{
    /* 主阻塞:等周期滴答信号量(无值),拿到就采一帧周期帧。 */
    if( xSemaphoreTake( xSampleSemaphore, pdMS_TO_TICKS( SAMPLE_POLL_TIMEOUT_MS ) ) == pdPASS )
    {
        prvTakeAndEnqueueSample( &ulSeq, &lValue, false );
    }

    /* 旁路 1:零超时探按钮信号量(无值),有就额外采一帧标 [BUTTON!]。 */
    if( xSemaphoreTake( xButtonSemaphore, 0 ) == pdPASS )
    {
        prvTakeAndEnqueueSample( &ulSeq, &lValue, true );
    }

    /* [10] 旁路 2:零超时探「自己的」任务通知值(带值),看控制源有没有下发新模式码。
     * 这里把任务通知当「带值的控制通道」用,正是它区别于上面那两把信号量的地方:
     * 信号量只能 give/take 一下、带不了码;任务通知把模式码直接写进通知值,一次送达。 */
    uint32_t ulNewMode = ulTaskNotifyTake( pdTRUE, 0 );
    if( ulNewMode == MODE_NORMAL || ulNewMode == MODE_BOOST )
    {
        if( ulNewMode != g_ulSampleMode )
        {
            g_ulSampleMode = ulNewMode;
            console_print( "sensor: mode switch via notify value=%lu -> %s\n",
                           ( unsigned long ) ulNewMode,
                           g_ulSampleMode == MODE_BOOST ? "BOOST" : "normal" );
        }
    }
}
```

注意这里把任务通知当「带值的控制通道」用,和上面那两把信号量(`xSampleSemaphore`、`xButtonSemaphore`)的「无值事件通道」放在一起,差别就一目了然了:同样是「零超时探一下」,信号量探到的是「有没有 token」(布尔),任务通知探到的是「控制源塞进来的模式码」(一个 32 位值)。sensor 此前的通知值一直空着,正好不和其他通道抢资源——这也是 dashboard 渐进生长的一个好处:每一章引入的原语都落在它最对口的位子上,不互相打架。构建运行:

```bash
cd code/dashboard
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/dashboard
```

输出长这样,周期采样(800ms)、按钮采样(2500ms)、模式切换(1600ms)三路来源交错出现,各自走各自的原语:

```
dashboard: starting scheduler (heap=2097152 bytes, sample=800 ms, button=2500 ms, init=1200 ms, control=1600 ms)
init: system initialization started
heap monitor: free=1308368  min_ever=1308368
sensor: sample #0 value=0 (periodic, mode=normal) sent to queue
init: initialization done, set BIT_INIT_DONE
display: showing sample #0 value=0 (sampled 401 ms ago, prev #0)
sensor: sample #1 value=1 (periodic, mode=normal) sent to queue
display: showing sample #1 value=1 (sampled 0 ms ago, prev #0)
control: sent notify value=1 (normal)
heap monitor: free=1439656  min_ever=1308368
stats: samples=2 (buttons=0)  displays=2
sensor: sample #2 value=2 (periodic, mode=normal) sent to queue
display: showing sample #2 value=2 (sampled 0 ms ago, prev #1)
sensor: sample #3 value=3 (BUTTON! extra sample, mode=normal) sent to queue
display: showing sample #3 value=3 (sampled 0 ms ago, prev #2)
sensor: sample #4 value=4 (periodic, mode=normal) sent to queue
display: showing sample #4 value=4 (sampled 0 ms ago, prev #3)
control: sent notify value=2 (BOOST)
sensor: mode switch via notify value=2 -> BOOST
sensor: sample #5 value=5 (periodic, mode=BOOST) sent to queue
display: showing sample #5 value=5 (sampled 0 ms ago, prev #4)
```

读这份输出时,把视线钉在 `control:` 和 `sensor: mode switch` 这两行上,你能看到任务通知「带值」的完整链路:第 1.6 秒 `control: sent notify value=2 (BOOST)` 这一行,就是控制源 `xTaskNotify` 把模式码 `2` 写进 sensor 通知值的那一刻;紧随其后的 `sensor: mode switch via notify value=2 -> BOOST`,是 sensor 在下一轮循环里 `ulTaskNotifyTake` 取到了这个 `2`、把 `g_ulSampleMode` 切成 BOOST 的那一刻——两行之间几乎没间隔,印证了任务通知的唤醒和取值极快。从 sample #5 往后,所有周期帧的 `mode=` 字段都变成了 `BOOST`,直到下一个 1600ms 控制源下发 `value=1` 切回 `normal`、帧又变回 `mode=normal`。这就是「带数据的任务通知」在一个真实多任务应用里的活体演示:模式码一路从控制源任务、经 sensor 的通知值、反映到每一帧采样的标记上,全程没有用到信号量、没有用到全局变量做带外通道。

再留意一下 `heap monitor` 那两行:启动时 `free=1308368`,几行之后变成 `free=1439656`、又稳定下来。这个起伏是 dashboard 各任务首次执行时按需分配的动态内存陆续到位、栈高水位稳定后的正常抖动,不是本章引入的额外开销——任务通知本身**不占对象 RAM**(它的通知值在 TCB 里),所以本章加的「控制源任务 + 模式切换」相比上一章,在 free heap 账本上几乎没有可感的下降,只多了一个 Control 任务自身的 TCB(几个任务都差不多大)。这恰好印证了前面讲过的「任务通知省一个同步对象」——要是本章用一把额外的信号量 + 一个全局变量来做模式切换,free heap 会再多掉一个信号量对象的量(几十字节),虽然不多,但任务通知连这点都省了。

## 小结

走到这里,任务通知的全貌就拼起来了。它的核心机制是**把一个 32 位通知值和一个通知状态直接塞进每个任务的 TCB**,所以它不需要创建独立的同步对象、天生定向(发给指定任务)、天生单点一对一——这三个特点决定了它既轻(RAM 零额外开销)又快(唤醒路径短、不遍历链表),是「点对点轻量通信」的尖兵。API 分两组:「无值组」`xTaskNotifyGive` + `ulTaskNotifyTake` 直接顶替二值/计数信号量;「带值组」`xTaskNotify(xTask, ulValue, eAction)` + `xTaskNotifyWait` / `ulTaskNotifyTake` 能往通知值里塞一个 32 位数据。`eAction` 的五种动作覆盖了所有常见语义——`eSetBits` 当轻量事件组、`eIncrement` 当轻量计数信号量、`eSetValueWithOverwrite` 当「最新值」指令信箱、`eSetValueWithoutOverwrite` 当「不丢消息」信箱、`eNoAction` 纯戳一下。

它最有卖点、也最该带走的能力,是**通知值能携带一个 32 位数据**——这是二值信号量结构上做不到的(信号量 give 一下就是一下,带不了码)。本章 demo 里控制源用 `eSetValueWithOverwrite` 把动作码/模式码写进工作任务的通知值、工作任务 `ulTaskNotifyTake` 取出来分支处理,「通知」和「数据」合一,干净利落地消掉了信号量版本「信号量 + 全局变量」那条带外通道及其竞态窗口。选型上记住一句话:**单点、一对一、要快、要省 → 用任务通知**;反过来,**多发送方灌消息、要广播、要缓冲一串 → 老老实实用队列、事件组、信号量**,任务通知不抢这些原语的地盘。而关于「更省 RAM、更快」,要诚实交代的是:host 模拟上你感觉不到这个差异(pthread 和 16KB 栈的开销压倒一切、PC 上也没有真实时性),我们演示的是用法和「带数据」的特性;这套「省、快」的结论是从真 MCU 的体量推出来的、搬到真硬件上完全成立。

下一章 [低功耗](../11_low-power/) 我们换一个画风:它是一个**诚实的边界章**——`configUSE_TICKLESS_IDLE` 和 tickless idle 在真 MCU 上是真省电的利器,但在我们这套 host 模拟下,PC 根本不在乎功耗、也不会真省,所以那一章我们重点讲机制和「真硬件上怎么用」,host 上只标注性地看一看 idle 的行为,把教学边界讲死。链回 [本章导航](./) 看全貌。
