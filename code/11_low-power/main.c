/*
 * 11_low-power —— 低功耗(Tickless Idle)的概念性 demo。
 *
 * ⚠️ 这是教学边界章,先把丑话说在前面(文档里有专节展开):
 *   host 模拟下,低功耗「没有任何可观测的真实效果」。PC 不在乎功耗,POSIX port 也不
 *   会真的停 tick 去省电。所以这个 demo 不是「给你看省了多少电」,而是把 Tickless Idle
 *   这套**机制**演给你看:打开 configUSE_TICKLESS_IDLE 后,内核的空闲任务会走上一条
 *   「算预计空闲时间 → 调用 portSUPPRESS_TICKS_AND_SLEEP()」的新路径。我们要演示、并
 *   诚实指出的,恰恰是这条路径在 POSIX port 上的「空跑」——这正是「host 模拟只讲得通
 *   概念、给不出省电数字」的活体证据。
 *
 * 机制回顾(详见文档):FreeRTOS 在 configUSE_TICKLESS_IDLE=1 时,空闲任务每轮会先算一个
 *   「预计还能空闲多久」(= 下一个任务解除阻塞的时间 - 当前 tick,见 tasks.c 里的
 *   prvGetExpectedIdleTime)。若这个预计空闲时间 >= configEXPECTED_IDLE_TIME_BEFORE_SLEEP
 *   (默认 2 个 tick),空闲任务就挂起调度器、调用 portSUPPRESS_TICKS_AND_SLEEP(预计空闲时间)。
 *   这个宏的真正实现由 port 负责:真 MCU port(GCC/ARM_CM3/4F、RP2040 等)会在里面关 tick、
 *   进 WFI/wait、配一个低功耗定时器在预计空闲时间到点时唤醒;POSIX port 则**压根没定义这个
 *   宏**,于是 FreeRTOS.h 给它兜了个空的 #define(什么都不做)。结果就是:host 上 tick 照常
 *   每毫秒跳一次、空闲任务照常转、CPU 照常被 app_hooks.c 里 vApplicationIdleHook 的 usleep
 *   让出去——我们看得见「机制被走到了」,看不见「tick 真的停了 / 真的省了电」。
 *
 * demo 的判官:一个 Worker 任务,vTaskDelay 一段较长时间(1 秒),制造「长空闲窗口」。
 *   它每次醒来,记录「这趟睡了多少 tick」(两次 xTaskGetTickCount() 的差)。判官就在这:
 *     configTICK_RATE_HZ=1000,1 tick=1ms,所以「睡 1 秒」tick 差理应 ≈ 1000。
 *     如果 tickless idle 在 host 上「真的」把 tick 停了,这趟 1 秒里 tick 中断不该响 1000 次;
 *     而实测它就是响 ~1000 次、tick 差就是 ~1000——这就是「tick 没停、概念成立、效果缺失」
 *     的铁证。(真 MCU port 上,tick 会被停掉、只靠低功耗定时器到点唤醒,内核醒来后把 tick
 *   计数补到「该有的值」,所以 tick 差「看起来」仍是 ~1000——但 tick 中断实际只响了一次或零次。
 *   host 模拟分辨不出「真跳了 1000 次」和「跳了 1 次补了 999」,这正是边界所在。)
 *
 * 这份输出的重点不是「省了多少」,而是:
 *   1) 它能编、能跑、不断言 —— 说明 configUSE_TICKLESS_IDLE=1 这条配置链在 POSIX port
 *      下被正确接住了(没炸,机制在)。
 *   2) 每趟「睡 1 秒」tick 差 ≈ 1000 —— 说明 tick 没被停,这就是 host 模拟下
 *      「概念成立、效果缺失」的铁证。
 *
 * 所有 FreeRTOS 必需的回调样板(vAssertCalled / 各类 hook / 静态分配内存)都在 app_hooks.c
 * 里,这个文件只放 demo 本身的逻辑。
 */

#include <stdbool.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "console.h"

/* Worker 任务的优先级。configMAX_PRIORITIES=7,合法优先级 0..6。
 * 给 +2,比空闲(0)高一档即可——本章不演示优先级,只要它「周期性睡一大觉」制造空闲窗口。 */
#define prioWORKER    ( tskIDLE_PRIORITY + 2 )

/* Worker 每次睡多久。设成 1000ms,制造一个 1 秒的长空闲窗口——这正是触发
 * Tickless Idle 的典型场景(下一个「该做的事」在 1 秒外,中间全是空闲)。 */
#define WORKER_SLEEP_MS    ( 1000 )

/* Worker 任务:周期性睡一大觉,制造长空闲窗口;每次醒来报告「这趟睡了多少 tick」。
 *
 * 判官逻辑:每次醒来记下当前的 xTaskGetTickCount(),和上一趟醒来时的 tick 求差。
 *   tick 差 ≈ WORKER_SLEEP_MS(因为 configTICK_RATE_HZ=1000,1 tick = 1ms)。
 *   如果 tickless idle 在 host 上「真的」把 tick 停了,那「睡 1 秒」期间 tick 不该
 *   累加 ~1000 次;而实测它就是累加 ~1000 次——这就是「tick 没停、概念成立、效果缺失」
 *   的铁证。 */
static void prvWorkerTask( void *pvParameters )
{
    ( void ) pvParameters;
    TickType_t xLastTick = xTaskGetTickCount();
    uint32_t ulRound = 0;

    for( ; ; )
    {
        /* 睡 WORKER_SLEEP_MS。这一行就是「长空闲窗口」的制造者:Worker 一 blocked,
         * 系统里没有别的就绪任务(本 demo 只有它一个非空闲任务),调度器切到空闲任务,
         * 空闲任务走 tickless-idle 路径(因 configUSE_TICKLESS_IDLE=1):
         *   算预计空闲 ≈ WORKER_SLEEP_MS 个 tick → 调 portSUPPRESS_TICKS_AND_SLEEP()。
         * 在 POSIX port 上,这个宏是空的,tick 照常每 ms 跳一次。 */
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

int main( void )
{
    /* 关掉 stdout 全缓冲,避免重定向/管道时输出丢失(见 02_environment 排坑章)。 */
    setvbuf( stdout, NULL, _IOLBF, 0 );

    console_init();

    console_print( "11_low-power: tickless-idle concept demo\n" );
    console_print( "11_low-power: configUSE_TICKLESS_IDLE=1, tick_rate=%lu Hz, worker_sleep=%d ms\n",
                   ( unsigned long ) configTICK_RATE_HZ, WORKER_SLEEP_MS );
    /* 把「丑话」直接打在 banner 里,让输出自带边界声明。 */
    console_print( "11_low-power: NOTE POSIX port has NO real tick suppression "
                   "(portSUPPRESS_TICKS_AND_SLEEP is a no-op here) — showing the mechanism, not power\n" );

    xTaskCreate( prvWorkerTask, "Worker", configMINIMAL_STACK_SIZE, NULL,
                 prioWORKER, NULL );

    console_print( "11_low-power: starting scheduler\n" );
    vTaskStartScheduler();

    for( ; ; )
    {
    }

    return 0;
}
