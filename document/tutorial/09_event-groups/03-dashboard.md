---
title: dashboard 增量:display 等「初始化完成 AND 有新采样」才刷新
description: 把事件组嫁接到贯穿全教程的 dashboard 脊柱上——给 display 加一道组合门,用事件组等「初始化完成 AND 有新采样」两个位都置位才显示一帧,演示 xClearOnExit 一刀切的局限与手动 ClearBits 的精细清除,以及常驻门 BIT_INIT_DONE 的活体用法,并附小结
---

# dashboard 增量:display 等「初始化完成 AND 有新采样」才刷新

← 返回 [事件组](./index.md)　·　上一页 [独立 demo](./02-demo.md)

把能力嫁接到贯穿全教程的 dashboard 脊柱上。dashboard 此前 sensor 每采一帧就经队列发给 display、display 收到就显示。本章增量是给 display 加一道**组合门**:在它动手显示一帧之前,先用事件组等「初始化完成 AND 有新采样」两个位都置位,只有两个条件都满足才往下走。这是一个非常真实的需求——你不会希望系统还在初始化(比如显示驱动没就绪)的时候 display 就贸然刷屏。我们用一块事件组 `xBootEvents`,上面两位:`BIT_INIT_DONE`(初始化完成,常驻门,一旦 set 不再清)和 `BIT_NEW_SAMPLE`(有新采样,sensor 每投一帧 set 一次,display 消费一帧清一次)。

先看 sensor 那一侧的改动——它在投完一帧后顺手 set 一下「新采样」位:

```c
xQueueOverwrite( xSampleQueue, &xFrame );

/* 投完帧,顺手 set 一下 BIT_NEW_SAMPLE,告诉 display「队列里有新货了」。 */
xEventGroupSetBits( xBootEvents, BIT_NEW_SAMPLE );
```

这一 set 就是 display AND 条件里「新采样」那一半的供给方。注意 set 位是在 sensor 的普通任务上下文里做的,不涉及 ISR,所以用普通的 `xEventGroupSetBits`,不需要 FromISR 版。

display 那一侧的改动是本章的戏肉,它把原来的「队列上死等」换成了「事件组上 AND 组合等待」:

```c
for( ; ; )
{
    /* AND 组合等待:BIT_INIT_DONE 和 BIT_NEW_SAMPLE 两个位「都」置位才解除阻塞。
     *   waitAll = pdTRUE —— AND;若要 OR 给 pdFALSE。
     *   clearOnExit = pdFALSE —— 故意不清,我们想在下面手动只清 BIT_NEW_SAMPLE、
     *     保留 BIT_INIT_DONE 这道常驻门。 */
    ( void ) xEventGroupWaitBits( xBootEvents,
                                  BIT_INIT_DONE | BIT_NEW_SAMPLE,
                                  pdFALSE,    /* 不在这里清,手动清 */
                                  pdTRUE,     /* AND:两个位都到位才返回 */
                                  portMAX_DELAY );

    /* 消费掉「新采样」标记:这一帧的 BIT_NEW_SAMPLE 我吃了,清掉;BIT_INIT_DONE 不动。 */
    xEventGroupClearBits( xBootEvents, BIT_NEW_SAMPLE );

    /* 队列里此刻一定有一帧(sensor 投帧时才 set 的 BIT_NEW_SAMPLE)。零超时取。 */
    SampleFrame_t xFrame;
    BaseType_t xGot = xQueueReceive( xSampleQueue, &xFrame, 0 );
    /* ... 显示这一帧 ... */
}
```

这里有个设计点值得专门讲,也是本章和独立 demo 的一个关键差别:我们给 `xClearOnExit` 传了 `pdFALSE`,然后**手动**用 `xEventGroupClearBits` 只清 `BIT_NEW_SAMPLE`。为什么不一刀切地用 `xClearOnExit=pdTRUE` 自动清?因为这里两个位的「生命周期」不一样——`BIT_NEW_SAMPLE` 是「一次性」的(每帧来了又走,该被消费),而 `BIT_INIT_DONE` 是「常驻」的(系统初始化好了就是好了,整个运行期间都该保持「已完成」状态)。如果用 `xClearOnExit=pdTRUE`,它会在第一帧显示成功后把 `BIT_INIT_DONE` 也一起清掉,那 display 下一轮的 AND 等待就再也满足不了(缺了初始化位),display 直接卡死。所以这里必须 `pdFALSE` + 手动只清新采样位——这正是 `xClearOnExit` 那个「一刀切」局限的活体演示,也顺带演示了 `xEventGroupClearBits` 的精细清除用法。

初始化位由谁 set?我们专门起一个 `InitTask`,延时一段模拟「初始化耗时」后 set 它:

```c
/* 初始化任务:延时 INIT_DONE_DELAY_MS 模拟「系统初始化要花点时间」,然后 set BIT_INIT_DONE。 */
static void prvInitTask( void *pvParameters )
{
    ( void ) pvParameters;
    console_print( "init: system initialization started\n" );
    vTaskDelay( pdMS_TO_TICKS( INIT_DONE_DELAY_MS ) );
    console_print( "init: initialization done, set BIT_INIT_DONE\n" );
    xEventGroupSetBits( xBootEvents, BIT_INIT_DONE );
    vTaskDelete( NULL );
}
```

我们**故意把 `INIT_DONE_DELAY_MS` 设成 1200ms,大于采样周期 800ms**。这个错位是精心安排的:sensor 的第一帧会在 800ms 时被采到、投进队列、并 set 了 `BIT_NEW_SAMPLE`,但此刻 `BIT_INIT_DONE` 还没到位(初始化要 1200ms),所以 display 的 AND 等待**不会**被解除——它会眼睁睁看着队列里有货、就是不动手显示,直到 1200ms 初始化完成、`BIT_INIT_DONE` 补上,AND 条件才齐,display 才被「解锁」开始显示积压的帧。这就是「AND 组合门」的活体演示:有新采样、但缺初始化,等于条件没齐,任务就继续 blocked。

构建运行(注意 dashboard 现在多了事件组和一个 Init 任务,脊柱照旧 build+run):

```bash
cd code/dashboard
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 5 ./build/dashboard
```

输出长这样,关键看头几行的时序:

```
dashboard: starting scheduler (heap=2097152 bytes, sample=800 ms, button=2500 ms, init=1200 ms)
init: system initialization started
heap monitor: free=1439656  min_ever=1439656
sensor: sample #0 value=0 (periodic) sent to queue
init: initialization done, set BIT_INIT_DONE
display: showing sample #0 value=0 (sampled 401 ms ago, prev #0)
sensor: sample #1 value=1 (periodic) sent to queue
display: showing sample #1 value=1 (sampled 0 ms ago, prev #0)
heap monitor: free=1570944  min_ever=1439656
stats: samples=2 (buttons=0)  displays=2
sensor: sample #2 value=2 (periodic) sent to queue
display: showing sample #2 value=2 (sampled 0 ms ago, prev #1)
sensor: sample #3 value=3 (BUTTON! extra sample) sent to queue
display: showing sample #3 value=3 (sampled 0 ms ago, prev #2)
sensor: sample #4 value=4 (periodic) sent to queue
display: showing sample #4 value=4 (sampled 0 ms ago, prev #3)
sensor: sample #5 value=5 (periodic) sent to queue
display: showing sample #5 value=5 (sampled 0 ms ago, prev #4)
heap monitor: free=1570944  min_ever=1439656
stats: samples=6 (buttons=1)  displays=6
```

盯住 sample #0 那一行——注意它的 `sampled 401 ms ago`。这条数字是全章最硬的证据:sample #0 是 sensor 在第 800ms 采的(`sent to queue` 那行先打出来),但它被 display 显示出来时已经是第 1200ms 初始化完成之后,所以「采样到显示」的延迟是 401ms,正好约等于 1200−800。这 400ms 的延迟**不是**采集链路慢,而是 display 被 AND 组合门卡住了——它有新采样(`BIT_NEW_SAMPLE` 已 set),但初始化位还没到,AND 条件不齐,它老老实实 blocked 等着。对比看 sample #1 及之后的全是 `sampled 0 ms ago`:一旦初始化完成、`BIT_INIT_DONE` 常驻到位,后面每一帧 display 都是「初始化位在位 + 新采样位一到」就立刻显示,延迟归零,采集→队列→显示的链路丝滑如初——说明我们这道组合门只在该卡的时候卡了第一帧,没把后续的实时性拖坏。再看 sample #3 那帧是 `[BUTTON!]`,说明 [07] 章加的按钮中断、[08] 章加的 mutex 统计这些增量全都照常工作,本章只是「在脊柱上又嫁接了一块事件组」,没有弄坏任何已有部件。

留意 `heap monitor` 的数:本章比上一章(08)又掉了一些 free heap,掉的部分就是新增的事件组对象 + Init 任务栈的量级。这是渐进脊柱的「账本」,到第 14 章集成时你会看到一条完整的下降曲线——每加一个原语,free heap 都相应少一点,但因为我们一开始就把 `configTOTAL_HEAP_SIZE` 抬到了 2MB(见 [03 堆内存管理](../03_memory/)),这点开销绰绰有余,绝不会触发 malloc failed。

## 小结

走到这里,事件组的全貌就拼起来了。**事件组是一块共享的布尔标志板**:每一位是一个独立的事件标志,任意多个任务都能 `xEventGroupSetBits` 置位、任意多个任务都能 `xEventGroupWaitBits` 等一个位的组合,setter 和 waiter 互不耦合——这种「一对多、多对一」的广播语义是队列、信号量、任务通知都给不了的。**`xEventGroupWaitBits` 的五个参数**里,`uxBitsToWaitFor` 是你关心的位掩码,`xWaitForAllBits` 是 AND(`pdTRUE`,全部到位才返回)还是 OR(`pdFALSE`,任一即返回)的开关,`xClearOnExit` 是成功返回时清不清位,`xTicksToWait` 是超时;返回值是解除阻塞那一刻的位快照,你拿它判断「是哪个位叫醒了我」「我是不是超时返回的」。**清除语义**有两条路:`xClearOnExit=pdTRUE` 让 wait 自动一刀切清掉等到的位(适合「事件是边沿、消费即清」),`xClearOnExit=pdFALSE` 配 `xEventGroupClearBits` 手动只清你想清的位(适合「位里有常驻状态、要精细控制」)——dashboard 增量里那道常驻的 `BIT_INIT_DONE` 就是后者的典型用例。

最该带走的判断力是那道**选择题**:传数据用队列,发单个事件给单个任务用信号量或任务通知(任务通知更轻),等多个事件的组合或广播给多个任务用事件组。事件组不传数据、不计数、不保证顺序,它只擅长「等多事件组合」这一件事,但这件事它做得比谁都干净。事件组本身是纯软件机制,host 模拟和真硬件行为完全一致,这章没有 host 特有的坑点(唯一的注意是 ISR 里 set 位要用 `xEventGroupSetBitsFromISR`,它走延迟处理,和中断章那套规矩一致,本章 demo 不涉及)。

---

下一章 [任务通知](../10_task-notifications/) 我们换一个更轻、更省的原语——它会让我们重新审视「同一个『发个事件』需求,信号量、事件组、任务通知三种写法各自的开销和取舍」,你会发现任务通知在「点对点单事件」这个最常见的小场景里,往往是性价比最高的那一个。
