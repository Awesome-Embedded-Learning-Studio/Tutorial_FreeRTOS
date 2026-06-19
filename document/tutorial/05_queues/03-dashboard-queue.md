---
title: dashboard 增量:把那根全局变量换成队列
description: 队列管理第三篇——把 dashboard 里 sensor 到 display 的裸全局变量换成「最新值寄存器」语义的队列(容量 1 + xQueueOverwrite),采集→队列→显示的完整通路,并附本章小结
---

# dashboard 增量:把那根全局变量换成队列

> 队列章第三篇,也是最后一篇。上一篇 [独立 demo](./02-blocking-demo.md) 演了「事件流」语义的队列(容量 4、阻塞与超时)。这一篇回到贯穿全教程的 dashboard 脊柱,把上一章遗留的裸全局变量换成队列——但用的是相反的「最新值寄存器」语义,并讲清为什么。结尾给整章收个小结。

## 把那根全局变量换成队列

学完独立 demo,我们回到贯穿全教程的 dashboard 脊柱。上一章它已经有 sensor 采集任务和 display 显示任务,但中间用一根裸全局变量 `g_xLatestSample` 传数据。本章增量就是**把那根全局变量换成队列**:sensor 每采到一帧,经队列发给 display;display 在队列上阻塞读,数据一进来就被唤醒显示。采集→队列→显示,这就是 spec 里规划的完整数据通路。

这里有个值得专门讲的设计选择。dashboard 的 sensor/display 是「传感器读数→给人看」的关系,它要的是**最新值语义**——display 该显示的是「此刻最新的那一帧」,积压几帧陈旧数据毫无意义(你不会希望 display 还在显示 2 秒前的旧读数)。所以我们不沿用独立 demo 那种「容量 4、`xQueueSend`」的「事件流」语义,而是用前面提到的「最新值寄存器」语义:**队列容量设成 1,投递用 `xQueueOverwrite`**。这样 sensor 每采一帧就覆盖掉队列里那条旧的,display 读到的永远是当前最新一帧,既不会积压陈旧数据,也不会因为 display 一时慢了就塞满队列、反过来卡住 sensor。

先看帧和队列。和 [上一章](../04_tasks/) 传单个 `int32_t` 不同,这里特意用**结构体**来传——既体现队列「按值拷贝整块结构体」的能力,也顺带把上一章遗留的「结构体会撕裂」隐患彻底解决:

```c
/* [05] sensor 采到、经队列发给 display 的一帧数据。
 * 用结构体而非单个整数,正是为了体现队列「按值拷贝整块结构体」的能力——
 * [04] 的裸全局变量传 int 还能侥幸,传结构体就会撕裂;队列没有这个问题。 */
typedef struct
{
    uint32_t ulSeq;       /* 帧序号,单调递增 */
    int32_t  lValue;      /* 模拟传感器读数(这里用一个递增计数器模拟) */
    uint32_t ulTick;      /* 采样时刻的 tick 数,相当时间戳 */
} SampleFrame_t;

/* [05] 采样队列句柄。进 scheduler 之前 xQueueCreate 创建好。 */
static QueueHandle_t xSampleQueue = NULL;
```

队列创建同样在 `main` 里、进 scheduler 之前,容量 1:

```c
/* [05] 采样队列必须在进 scheduler 之前创建好。容量 1、每帧一个 SampleFrame_t。
 * 配合 sensor 的 xQueueOverwrite,它就是个「最新值寄存器」。
 * 返回 NULL 是致命错误(heap 不够),直接断言。 */
xSampleQueue = xQueueCreate( SAMPLE_QUEUE_LENGTH, sizeof( SampleFrame_t ) );
configASSERT( xSampleQueue != NULL );
```

sensor 任务的采集逻辑和上一章几乎一样,只是把「写全局变量」换成「`xQueueOverwrite` 进队列」:

```c
xFrame.ulSeq = ulSeq;
xFrame.lValue = lValue;
xFrame.ulTick = xTaskGetTickCount();

/* xQueueOverwrite:队列满(容量 1)时,丢掉最旧的那帧、写入新帧,绝不阻塞。
 * 它把「保留最新值」这件事做成了原子操作,正好是「最新值寄存器」语义。 */
xQueueOverwrite( xSampleQueue, &xFrame );

console_print( "sensor: sample #%lu value=%ld sent to queue\n",
               ( unsigned long ) ulSeq, ( long ) lValue );
```

display 任务的变化才是这一增量的重点。上一章它靠 `vTaskDelay(1000)` 盲等,每秒醒一次去读全局变量;这一章它在队列上**阻塞读**,而且用 `portMAX_DELAY` 死等——因为对 display 来说「这一帧我非收不可」,不必超时降级:

```c
/* portMAX_DELAY:无超时、死等。队列空就 blocked,直到 sensor 投递一帧才被唤醒。
 * 这一行就是 [05] 最核心的变化——display 的 blocked 现在由「队列里有数据」驱动,
 * 而不是 [04] 那样由 vTaskDelay 定时驱动。 */
BaseType_t xGot = xQueueReceive( xSampleQueue, &xFrame, portMAX_DELAY );

if( xGot == pdPASS )
{
    /* 读到的是队列里那帧的完整副本,不会有 [04] 全局变量的撕裂风险。
     * 顺便显示这一帧从采样到被显示的延迟(tick 差),验证「几乎实时」。 */
    console_print( "display: showing sample #%lu value=%ld (sampled %lu ms ago, prev #%lu)\n",
                   ... );
    ulLastShown = xFrame.ulSeq;
}
```

`sampled X ms ago` 那个延迟,是用当前 tick 减去帧里记的采样 tick 算出来的——这一章你会看到它几乎一直是 0,说明 display 是被数据唤醒、近实时显示的,而不是像上一章那样定时轮询、可能错过中间好几帧。构建运行:

```bash
cd code/dashboard
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/dashboard
```

输出长这样,sensor 采一帧、立刻 `sent to queue`,display 同一毫秒就被唤醒显示:

```
dashboard: starting scheduler (heap=2097152 bytes)
sensor: sample #0 value=0 sent to queue
display: showing sample #0 value=0 (sampled 0 ms ago, prev #0)
heap monitor: free=1703072  min_ever=1703072
sensor: sample #1 value=1 sent to queue
display: showing sample #1 value=1 (sampled 0 ms ago, prev #0)
sensor: sample #2 value=2 sent to queue
display: showing sample #2 value=2 (sampled 0 ms ago, prev #1)
heap monitor: free=1703072  min_ever=1703072
sensor: sample #3 value=3 sent to queue
display: showing sample #3 value=3 (sampled 0 ms ago, prev #2)
sensor: sample #4 value=4 sent to queue
display: showing sample #4 value=4 (sampled 0 ms ago, prev #3)
```

对比上一章(裸全局变量)的输出,你会发现最关键的差别在节奏上。上一章 sensor 是 800ms 周期、display 是 1000ms 周期,两者各跑各的,display 经常读到一个和上次相同的值(因为 sensor 还没采下一帧),甚至可能跳帧——因为没有任何机制保证 display 看到每一帧。这一章 sensor 还是 800ms 采一次,但 display **不再有自己的周期**,它完全由队列驱动:sensor 一投递,display 立刻被唤醒显示,所以输出里 sensor 和 display 严格成对、`sampled 0 ms ago`。这一次「一帧不丢、近实时显示」,就是队列把同步和数据传递一起做好之后的直接成果——上一章那个「display 可能跳帧、可能读到半新半旧」的隐患,到这里被根治了。

还有个细节值得品:display 显示完一帧后,`xQueueReceive` 立刻又进 blocked 等下一帧。如果 display 这一刻恰好还没显示完、sensor 就投了新帧怎么办?因为用的是 `xQueueOverwrite` + 容量 1,新帧会覆盖掉队列里那条还没被取走的——display 醒来时拿到的是**最新的**那一帧,中间被覆盖掉的旧帧就跳过了。这正是「最新值语义」:我们**故意**接受这种跳帧,因为对「传感器读数显示」来说,「永远显示最新值」比「一帧不丢」重要。如果你的场景是「每一条都不能少」(比如命令/事件流),就该像独立 demo 那样用容量大于 1 的 `xQueueSend`。这两种语义的取舍,是队列用得好不好的分水岭。

`heap monitor: free=1703072` 这行也值得对照读:它和上一章(只有 sensor+display 两个应用任务、还没队列时)的值很接近——本章只是把全局变量换成了队列,没加新任务。队列本身吃掉的堆很少(一个容量 1、每帧 12 字节的队列,加上控制块也就几十字节),远不到「一个任务栈 128KB」那个量级。这印证了上一章讲的「堆的大头是任务栈,内核对象零头」:随着后续章节我们继续往 dashboard 里加定时器、信号量、互斥量,`free` 还会缓慢往下走,但主要还是被任务栈吃掉的,队列这类对象只占零头。

## 小结

走到这里,队列管理的全貌就拼起来了:**队列是一根线程安全、带阻塞通知的环形传送带**,一头塞一头取,内核帮你管并发和等待;**`xQueueCreate(长度, 单条大小)` 从堆里搭起它**——第二个参数是消息本身的大小不是指针大小,因为队列是**按值拷贝**的,投递时 `memcpy` 进缓冲区、取出时 `memcpy` 出来,这保证了数据完整性、彻底解耦了发送方和接收方的内存所有权,代价是多一次拷贝(对几十字节的小消息可忽略);**`xQueueSend` 的第三参数决定队列满时怎么办**——死等、有限超时、还是立刻返回 `errQUEUE_FULL`,返回值必须看否则会默默丢消息;**`xQueueReceive` 的阻塞是队列最值钱的能力**——空队列上接收任务进 blocked、不占 CPU,数据一来就被唤醒近实时消费,这一下把「同步」从你手里接了过去,比定时轮询既及时又省;**超时返回是另一面**——等不到数据就在约定时间后醒来报告,而不是被永久卡死;**「最新值」与「一帧不丢」是两种相反语义**——前者用容量 1 的 `xQueueOverwrite`(队列当单槽最新值寄存器),后者用容量大于 1 的 `xQueueSend`,选哪个看你的数据是「最新重要」还是「每条重要」。在 dashboard 上,我们把上一章那根有撕裂风险的裸全局变量,换成了 sensor→队列→display 的完整通路,既治好了竞态、又把 display 从定时轮询升级成了事件驱动。

下一章 [软件定时器](../06_timers/) 我们换一个角度——sensor 现在还是靠 `vTaskDelay(800)` 自己定时轮询来决定采样时刻,这种「任务自己掐表」其实不够干净;软件定时器能把「到点了做某件事」这件事从任务里抽出来、交给内核的定时器服务任务统一管,届时我们会用它来驱动 sensor 的采样周期。至于「host 模拟和真 MCU 在队列行为上的差异」——比如拷贝开销的实际体感、队列满了的表现——属于系统性的仿真坑点,收在 [仿真坑点](../../pitfalls/) 里,值得对照本章读一遍。也可以回到本章的 [总览导航](./index.md)。
