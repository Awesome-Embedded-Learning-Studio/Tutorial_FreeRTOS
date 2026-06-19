---
title: 独立 demo:打开 configUSE_TICKLESS_IDLE,亲眼看见「机制在跑、tick 没停」
description: 低功耗章 demo 篇——一个只睡大觉的 Worker 任务制造长空闲窗口,演示打开 configUSE_TICKLESS_IDLE=1 后整套机制在 POSIX port 下被正确接住(能编能跑不断言),并用「每趟睡 1 秒 tick 差仍 ≈ 1000」这个铁证证明 host 上 tick 没有被停,从而让「概念成立、效果缺失」从口头声明变成可看的数据
---

# 独立 demo:打开 configUSE_TICKLESS_IDLE,亲眼看见「机制在跑、tick 没停」

> 低功耗章第二页。上一页 [Tickless Idle 的机制](./01-tickless-idle-concept.md) 把概念拆透了,这一页配上能编能跑的 demo,把「host 上机制在跑、tick 没停」演成可观测的数据。下一页 [真硬件两个坑与 dashboard 增量取舍](./03-real-hardware-and-dashboard.md) 把视角拉回真 MCU 并交代为什么这一章不动 dashboard。

概念讲到位,我们写一个 demo 把上面这件事**可观测地**演出来。注意我们演的不是「省了多少电」(那在 host 上测不出来),而是两件能编能跑、看得见的事:其一,打开 `configUSE_TICKLESS_IDLE=1` 后,这整套机制在 POSIX port 下被正确接住了——它能编、能跑、不断言,说明内核的空闲任务确实走上了那条新路径,没有因为 port 没实现宏就炸;其二,我们要拿出一个铁证,证明「host 上 tick 没有被停」,从而让「概念成立、效果缺失」这件事从口头声明变成可看的数据。demo 在仓库的 `code/11_low-power/` 里。

demo 的设计很精简。它只有一个 Worker 任务,核心动作就是「睡一大觉」——`vTaskDelay` 一整秒。这一行 `vTaskDelay` 就是「长空闲窗口」的制造者:Worker 一进 blocked,系统里再没有别的就绪任务,调度器切到空闲任务,空闲任务因为 `configUSE_TICKLESS_IDLE=1` 开始走那条「算预计空闲(≈1000 个 tick)→ 判门槛(够长)→ 挂起调度器 → 调 `portSUPPRESS_TICKS_AND_SLEEP(1000)`」的路径。Worker 每次醒来,我们记录「这趟睡了多少 tick」——用两次 `xTaskGetTickCount()` 求差:

```c
/* Worker 任务:周期性睡一大觉,制造长空闲窗口;每次醒来报告「这趟睡了多少 tick」。
 * 判官就在这:configTICK_RATE_HZ=1000,1 tick=1ms,所以「睡 1 秒」tick 差理应 ≈ 1000。
 * 如果 tickless idle 在 host 上「真的」把 tick 停了,这趟 1 秒里 tick 不该累加 ~1000 次;
 * 而实测它就是累加 ~1000 次——这就是「tick 没停、概念成立、效果缺失」的铁证。 */
static void prvWorkerTask( void *pvParameters )
{
    ( void ) pvParameters;
    TickType_t xLastTick = xTaskGetTickCount();
    uint32_t ulRound = 0;

    for( ; ; )
    {
        /* 睡一整秒。这一行制造「长空闲窗口」:Worker 一 blocked,系统只剩空闲任务,
         * 空闲任务走 tickless-idle 路径(算预计空闲 → 调 portSUPPRESS_TICKS_AND_SLEEP())。
         * 在 POSIX port 上,那个宏是空的,tick 照常每 ms 跳一次。 */
        vTaskDelay( pdMS_TO_TICKS( WORKER_SLEEP_MS ) );

        TickType_t xNowTick = xTaskGetTickCount();
        TickType_t xDeltaTicks = xNowTick - xLastTick;
        xLastTick = xNowTick;

        console_print( "worker: round %lu woke up — slept %lu ticks (~%lu ms)\n",
                       ( unsigned long ) ulRound,
                       ( unsigned long ) xDeltaTicks,
                       ( unsigned long ) ( xDeltaTicks * portTICK_PERIOD_MS ) );
        ulRound++;
    }
}
```

这里的判官就是那个 `xDeltaTicks`。`configTICK_RATE_HZ=1000`,所以 1 个 tick 正好是 1ms;Worker 喊着要睡 1000ms,那 `xDeltaTicks` 理论上就该是 1000。关键就在于:**如果 `portSUPPRESS_TICKS_AND_SLEEP` 真的把 tick 停了**,那这趟 1 秒的睡眠里,tick 中断不该响 1000 次、`xTaskGetTickCount()` 也不该真的累加 1000 次(真 MCU port 上,它是醒来后 port 帮你「补」回去的,tick 中断实际只响了一次或零次;但 host 上根本没有这个「补」的机制,因为宏是空的)。所以 host 上如果 tick 差还是 ~1000,就反向证明了一件事:**tick 根本没被停过,它老老实实跳了 1000 次,只是内核照常把它们累加进 tick 计数而已**。这就是我们要的铁证。

构建和运行还是老配方(`stdbuf -oL`、`timeout` 的来历全在 [02 环境搭建](../02_environment/) 讲过):

```bash
cd code/11_low-power
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/11_low-power
```

输出长这样:

```
11_low-power: tickless-idle concept demo
11_low-power: configUSE_TICKLESS_IDLE=1, tick_rate=1000 Hz, worker_sleep=1000 ms
11_low-power: NOTE POSIX port has NO real tick suppression (portSUPPRESS_TICKS_AND_SLEEP is a no-op here) — showing the mechanism, not power
11_low-power: starting scheduler
worker: round 0 woke up — slept 1000 ticks (~1000 ms)
worker: round 1 woke up — slept 1000 ticks (~1000 ms)
worker: round 2 woke up — slept 1000 ticks (~1000 ms)
```

读这份输出,要抓两个点。第一,banner 那行 `NOTE POSIX port has NO real tick suppression` 是我们故意把丑话说在输出的最前面,免得你跑完兴冲冲地以为「我看见省电了」——没有,host 上不可能看见。第二,也是真正的判官:每一轮 Worker 都报告 `slept 1000 ticks (~1000 ms)`。它喊睡 1000ms,tick 差就老老实实是 1000——这恰恰说明这 1000ms 里 tick 中断响了整整 1000 次、压根没被停。如果今天是在一个真 MCU port 上跑、且 tickless idle 真的生效了,这个 tick 差「看起来」也还是 1000(因为 port 醒来后把 tick 补回来了),所以单看这个数字你**无法**在 host 上区分「tick 真跳了 1000 次」和「tick 跳了 1 次补了 999」——这种分辨能力 host 模拟给不了你。但这不削弱结论,反而坐实了它:我们证明的是「host 上 tick 确实在老老实实地跳」(因为它没有停 tick 的机制),而不是「tickless idle 没用」。tickless idle 在真 MCU 上当然有用、有大用,只是 host 测不出来。

demo 演完了 host 的边界,下一页 [真硬件两个坑与 dashboard 增量取舍](./03-real-hardware-and-dashboard.md) 把视角拉回真 MCU——讲两个真硬件上用 tickless idle 必踩的坑,并交代为什么这一章我们不动贯穿全教程的 dashboard 脊柱。
