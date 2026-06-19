---
title: 独立 demo:把阻塞与超时演给你看
description: 队列管理第二篇——code/05_queues 独立 demo,一个 Sender 周期投帧、一个 Receiver 有限超时阻塞读,在真实输出里同时演「被唤醒消费」与「空窗超时返回」
---

# 独立 demo:把阻塞与超时演给你看

> 队列章第二篇。上一篇 [概念篇](./01-queue-concepts.md) 把 `xQueueReceive` 的阻塞语义讲透了,这一篇配一个能跑的 demo,把「阻塞读被唤醒」和「超时返回」用肉眼可见的输出亲眼演出来。demo 看完,下一篇 [dashboard 增量](./03-dashboard-queue.md) 把这套能力嫁接到贯穿全教程的 dashboard 上。

## demo 的安排

概念讲完,我们写一个能跑的 demo,专门把「阻塞读被唤醒」和「超时返回」这两件事用肉眼可见的输出演出来。demo 在仓库的 `code/05_queues/`,它的安排是:一个 Sender 任务(优先级较高)周期性地往队列里投带时间戳的采样帧;一个 Receiver 任务(优先级更低)在队列上阻塞读。Sender 会切换两种节奏——先连续快发(Receiver 每次都能在超时前被队列唤醒、立刻消费),然后故意停发一段比 Receiver 超时还长的空窗(队列被抽空、没人投递,Receiver 就在空队列上等满超时、超时返回),循环往复。这样一份输出里你能同时看到「有数据时被唤醒」和「空窗时超时」两种情形。

先看队列和帧的定义。队列容量故意只放 4 帧,每帧一个带序号和 tick 时间戳的小结构体:

```c
/* 投递进队列的一帧数据。就是个带序号和 tick 时间戳的小结构体——
 * 关键点:我们靠队列的「按值拷贝」语义投递它,Sender 投完就能函数返回,
 * 不用担心 Receiver 什么时候来取、取的时候那块内存还在不在。 */
typedef struct
{
    uint32_t ulSeq;     /* 帧序号,单调递增,方便在输出里追踪 */
    uint32_t ulTick;    /* 投递时刻的 tick 数(xTaskGetTickCount),相当时间戳 */
} SampleFrame_t;

/* 队列句柄。在进 scheduler 之前 xQueueCreate 创建好,任务里直接用。 */
static QueueHandle_t xSampleQueue = NULL;
```

`SampleFrame_t` 只有 8 字节,但它是结构体——你要是像 [上一章](../04_tasks/) 那样用裸全局变量传它,就已经有撕裂风险了。这里靠队列的按值拷贝,投递时整块 `memcpy` 进环形缓冲区、取出时整块 `memcpy` 出来,完整快照,没有撕裂。

队列的创建在 `main` 里、`vTaskStartScheduler` 之前完成,返回 `NULL` 就断言(heap 不够是致命错误):

```c
/* 队列必须在进 scheduler 之前创建好。一次 xQueueCreate 开出来:
 * 一个内部环形缓冲区,每个槽位 sizeof(SampleFrame_t) 字节,共 QUEUE_LENGTH 个槽。
 * 返回 NULL 表示创建失败(heap 不够)——这种致命错误直接断言。 */
xSampleQueue = xQueueCreate( QUEUE_LENGTH, sizeof( SampleFrame_t ) );
configASSERT( xSampleQueue != NULL );
```

## 投递与停发:Sender 这一侧

然后是核心的投递。Sender 填好一个**栈上的局部结构体** `xFrame`,直接 `xQueueSend` 投出去,第三个参数传 `0` 表示「满了就立刻放弃,不阻塞」——因为消费者跟得上,正常不会满;真满了也会明明白白报出来,而不是默默丢:

```c
xFrame.ulSeq = ulSeq;
xFrame.ulTick = xTaskGetTickCount();

/* xQueueSend 的第二个参数是「待拷贝数据的地址」,不是数据本身——
 * 内核会从这块地址 memcpy sizeof(SampleFrame_t) 字节进队列。
 * 第三个参数给 0:队列满了就立刻放弃(返回 errQUEUE_FULL),不阻塞。 */
BaseType_t xOk = xQueueSend( xSampleQueue, &xFrame, 0 );

prvPrintTickPrefix();
if( xOk == pdPASS )
{
    console_print( "sender   send  seq=%lu\n", ( unsigned long ) ulSeq );
}
else
{
    /* 队列满:消费端没跟上才会发生,报出来比静默丢帧强。 */
    console_print( "sender   send  seq=%lu  QUEUE FULL (dropped)\n",
                   ( unsigned long ) ulSeq );
}
```

注意那个 `&xFrame`——我们传的是栈上局部变量的地址,投完函数还在循环里、栈帧还活着;就算它哪天不在了,Receiver 拿到的也是副本,照样能读到完整一帧。这就是上一篇讲的「按值拷贝」在实战里保护你。Sender 投完 `BURST_FRAMES` 帧后,会进入停发空窗:

```c
/* 停发阶段:停 SILENCE_MS 毫秒。这段时间队列会被 Receiver 抽空、没人投递,
 * Receiver 就会在空队列上等满超时——超时返回这一幕就发生在这个空窗里。 */
prvPrintTickPrefix();
console_print( "sender   goes SILENT for %d ms (expect receiver timeouts)\n",
               SILENCE_MS );
vTaskDelay( pdMS_TO_TICKS( SILENCE_MS ) );
```

## 有限超时阻塞读:Receiver 这一侧

Receiver 那一侧就是本章最应该看懂的那一行——有限超时的阻塞读:

```c
/* 核心:第三个参数给有限超时。队列空就 blocked 等,最多等 RECEIVE_TIMEOUT_MS;
 * 期间有数据进来就被唤醒、拿到数据返回 pdPASS;
 * 若一直没数据,等满超时返回 errQUEUE_EMPTY(就是 pdFALSE)。 */
BaseType_t xGot = xQueueReceive( xSampleQueue, &xFrame,
                                 pdMS_TO_TICKS( RECEIVE_TIMEOUT_MS ) );

prvPrintTickPrefix();
if( xGot == pdPASS )
{
    console_print( "receiver got seq=%lu (queued %lu ms ago)\n",
                   ( unsigned long ) xFrame.ulSeq,
                   ( unsigned long ) ( xTaskGetTickCount() - xFrame.ulTick ) );
}
else
{
    /* 这一行就是「超时返回」的铁证:队列空了 RECEIVE_TIMEOUT_MS 还没数据。 */
    console_print( "receiver TIMEOUT: queue empty for %d ms\n",
                   RECEIVE_TIMEOUT_MS );
}
```

这里 `RECEIVE_TIMEOUT_MS` 定成 1500,是精心挑的:Sender 快发阶段每 300ms 来一帧,Receiver 总能在超时前被唤醒;而 Sender 进入 3000ms 的停发空窗后,Receiver 就会真的等满 1500ms 超时。`queued X ms ago` 那个 `X` 是「这一帧从投递到被消费排了多久」,用当前 tick 减去帧里记的投递 tick 算出来——这个数贴近 0,说明 Receiver 几乎是数据一进来就被唤醒的,正是阻塞读的效果。构建运行还是老配方,只是 demo 目录换成 `code/05_queues`:

```bash
cd code/05_queues
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 7 ./build/05_queues
```

(命令里那几个开关、`stdbuf -oL` 为什么不能少、`timeout` 怎么回事,全在 [02 环境搭建](../02_environment/) 里讲过,这里不重复。)运行起来你会看到这样的输出:

```
05_queues: queue length=4, frame size=8 bytes, receive timeout=1500 ms
05_queues: starting scheduler
[    1 ms] sender   send  seq=0
[    1 ms] receiver got seq=0 (queued 0 ms ago)
[  301 ms] sender   send  seq=1
[  301 ms] receiver got seq=1 (queued 0 ms ago)
[  601 ms] sender   send  seq=2
[  601 ms] receiver got seq=2 (queued 0 ms ago)
[  901 ms] sender   send  seq=3
[  901 ms] receiver got seq=3 (queued 0 ms ago)
[ 1201 ms] sender   send  seq=4
[ 1201 ms] receiver got seq=4 (queued 0 ms ago)
[ 1501 ms] sender   send  seq=5
[ 1501 ms] receiver got seq=5 (queued 0 ms ago)
[ 1801 ms] sender   goes SILENT for 3000 ms (expect receiver timeouts)
[ 3001 ms] receiver TIMEOUT: queue empty for 1500 ms
[ 4501 ms] receiver TIMEOUT: queue empty for 1500 ms
[ 4801 ms] sender   send  seq=6
[ 4801 ms] receiver got seq=6 (queued 0 ms ago)
[ 5101 ms] sender   send  seq=7
[ 5101 ms] receiver got seq=7 (queued 0 ms ago)
[ 5401 ms] sender   send  seq=8
[ 5401 ms] receiver got seq=8 (queued 0 ms ago)
```

## 读这份输出

这份输出值得逐行读。先看快发阶段(seq=0 到 seq=5):Sender 每投一帧,Receiver 几乎**同一毫秒**就 `got` 了它,`queued 0 ms ago`——这就是阻塞读的效果,数据一进队列,Receiver 立刻被内核唤醒取走,排队时间几乎为零。Sender 优先级更高,所以每个 tick 上它先跑、投完进 `vTaskDelay` 让出 CPU,Receiver 才被调度、立刻取走刚刚那条,于是两者在输出里成对出现、几乎贴在一起。这条「投递→立刻消费」的紧耦合,正是队列把同步做对了的样子。

然后是关键的那一段。1801ms 时 Sender 打了 `goes SILENT for 3000 ms`,之后整整 3000ms 没人投递。注意看 Receiver 在这段时间干了什么:它在 1801ms 取完 seq=5 之后,立刻又 `xQueueReceive` 进了 blocked——这次队列空了。它不会傻等占 CPU,而是睡着等最多 1500ms;1500ms 后(也就是 1801+1500≈3001ms)还没等到数据,就**超时返回**,打了那行 `receiver TIMEOUT: queue empty for 1500 ms`。然后它循环回去再 `xQueueReceive` 一次,又等 1500ms,到 4501ms 再次超时——于是你在输出里看到**连续两次超时**。这两行 `TIMEOUT` 就是「超时返回」这一幕的铁证:队列空了、Receiver 没有被永久卡死,而是在约定的时间后自己醒来、报告「这段时间没人投递」。4801ms Sender 重新开始投(seq=6),Receiver 立刻又被唤醒、恢复成 `queued 0 ms ago` 的紧耦合消费。

把整段连起来看,你就理解了队列阻塞读的全部行为:**有数据时,接收任务被数据唤醒、近实时消费;没数据时,接收任务在 blocked 里不占 CPU,要么等来数据、要么等来超时**。这两种情形在同一份输出里都演到了,这就是本章 demo 想让你带走的东西。还有个细节:队列容量 4、消费又这么及时,所以全程没出现 `QUEUE FULL`——如果哪天你在自己的程序里看到那行,说明消费端跟不上生产端,要么加大队列、要么让生产端降速、要么接受丢消息,这就是「队列满」要你做的工程决策。

下一篇 [dashboard 增量](./03-dashboard-queue.md) 我们把这套能力用到贯穿全教程的 dashboard 上,把上一章那根裸全局变量换成队列。
