---
title: dashboard 增量:拆成 sensor + display
description: 04 章 dashboard 篇——把 dashboard 的单一心跳任务拆成 sensor 采集任务 + display 显示任务(不同优先级),用裸全局变量 g_xLatestSample 临时过渡,观测堆随任务数下降,并把多任务骨架立起来,为下一章用队列替换裸全局变量埋伏笔;末尾小结任务管理全貌
---

# dashboard 增量:拆成 sensor + display

> 上一篇 [独立 demo](./02-demo.md) 跑通了优先级调度;这一篇把同样的能力嫁接到贯穿全教程的 dashboard 脊柱上。本章小结也收在这一篇末尾。

## 把心跳拆成两任务

学完独立 demo,我们把这个能力嫁接到贯穿全教程的 dashboard 脊柱上。dashboard 此前(02、03 章)只有一个心跳任务加一个内存监控任务,本章增量是**把单一心跳拆成 sensor 采集任务 + display 显示任务**——一个负责「采样」、一个负责「显示」,优先级不同,正式把 dashboard 从「单任务」推进到「多任务协作」的形态,为后续章节搭好骨架。

这里有个有意为之的设计选择。sensor 任务优先级更高(`tskIDLE_PRIORITY + 3`),因为「该采样时就要采样」,采样是时序敏感的,不能被挤掉;display 任务优先级更低(`tskIDLE_PRIORITY + 2`),因为显示是给人看的、对时序不敏感,压低优先级让它在 sensor 不抢 CPU 时再跑。两任务通过一个共享全局变量 `g_xLatestSample` 传递最新读数。我得先说清楚:**这个全局变量只是本章的临时过渡**,它能在这种「单写者+单读者、都是 32 位整数、读写基本原子」的场景下侥幸不出大问题,但一旦换成立结构体、或多个写者,裸全局变量就会 race。下一章我们专门用队列把它换掉,届时会讲透「为什么裸全局变量不安全、队列怎么解决」——现在先用它把多任务骨架立起来。sensor 任务的核心就这几行:

```c
/* [04] sensor 采集任务:每 800ms 采集一帧(用递增计数器模拟),写进共享变量。 */
static void prvSensorTask( void *pvParameters )
{
    ( void ) pvParameters;
    int32_t lSample = 0;

    for( ; ; )
    {
        g_xLatestSample = lSample;
        console_print( "sensor: sample #%ld collected\n", ( long ) lSample );
        lSample++;
        vTaskDelay( pdMS_TO_TICKS( 800 ) );
    }
}
```

display 任务对称地每秒把最新一帧显示出来,顺带打出「上一帧显示的是几号」,这样你能看到采集和显示的节奏差异。构建运行:

```bash
cd code/dashboard
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/dashboard
```

输出长这样,sensor(800ms 周期)比 display(1000ms 周期)更密集,采集优先于显示:

```
dashboard: starting scheduler (heap=2097152 bytes)
sensor: sample #0 collected
display: showing sample #0 (prev shown #-1)
heap monitor: free=1703272  min_ever=1703272
sensor: sample #1 collected
display: showing sample #1 (prev shown #0)
sensor: sample #2 collected
display: showing sample #2 (prev shown #1)
heap monitor: free=1703272  min_ever=1703272
sensor: sample #3 collected
display: showing sample #3 (prev shown #2)
sensor: sample #4 collected
```

注意看 `heap monitor: free=1703272`。在上一章(只有心跳 + 监控两个应用任务)时这个数是 `1834560`,本章多了 sensor/display(替换掉心跳)后掉到了 `1703272`——差了约 131KB,正好是一个任务栈(128KB + 开销)的量级。这就是上一章讲的「每多一个任务,堆就多掉一个栈」的直接观测,三个应用任务吃掉了约三个 128KB 栈。随着后续章节我们继续往 dashboard 里加队列、定时器、信号量、互斥量,你会看到这个 `free` 还会缓慢往下走——内存监控任务这个「常驻水位计」就是给我们留的长期视角。

还有个细节值得品:display 显示 `sample #2` 时,sensor 其实可能已经采到 `#2` 但还没采 `#3`,也可能已经跳了——因为我们用的是裸全局变量、没有「保证读到完整一帧」的机制。这种「可能丢帧、可能读到半新半旧」的隐患,正是下一章用队列要根治的问题。本章我们先把「两个任务、不同优先级、各跑各的」这套骨架立起来,数据正确性留到下一章解决。

## 小结

走到这里,任务管理的全貌就拼起来了:**`xTaskCreate` 的六个参数**分别是任务函数(永不返回)、名字(调试用)、栈深度(单位是字)、参数、优先级、句柄输出;**优先级是抢占式调度的核心**,任何时刻调度器都让「ready 且优先级最高」的任务跑,同优先级默认时间片轮转,数字越大越重要,但 0 留给 idle;**状态机**在 ready/running/blocked/suspended/deleted 之间流转,其中 blocked 是「主动让出 CPU」的好状态,善用 `vTaskDelay` 和阻塞 API 是多任务并存的前提,而高优先级任务不让出会饿死低优先级;**`vTaskDelete` 销毁任务**,POSIX port 下内存回收有延迟(真 MCU 更直接);**`configMINIMAL_STACK_SIZE` 是 idle/timer 的默认栈深和常用基准**,真正的栈大小由你传给 `xTaskCreate` 的 `usStackDepth` 决定,用 `uxTaskGetStackHighWaterMark` 探峰值再收紧。host 模拟独有的体量错觉依然在:所有任务栈都虚胖到 128KB,真 MCU 上是几百到几千字节的数量级。

下一章 [队列管理](../05_queues/) 我们正式进入任务间通信——把 dashboard 里那根不安全的裸全局变量 `g_xLatestSample` 换成队列,sensor 任务把采样数据经队列发给 display 任务,既解决丢帧和竞态,又顺带把「阻塞读」这种 blocked 的新来源讲清楚。至于「host 模拟和真 MCU 在任务行为上的差异」——栈双重性、`vTaskDelete` 回收延迟、忙等饿死——属于系统性的仿真坑点,收在 [仿真坑点](../../pitfalls/) 里,值得对照本章读一遍。
