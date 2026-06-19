/*
 * 06_timers —— 软件定时器独立 demo。
 *
 * 要把软件定时器这一个原语讲透,我们让两个定时器同台演出,把「单次」和「周期」
 * 的差别直接演给你看:
 *   1. 一个周期定时器(periodic):每 500ms 触发一次,周而复始,触发一次回调。
 *   2. 一个单次定时器(one-shot):启动 2 秒后才触发第一次、也是唯一一次回调,
 *      触发完就再也不响了——它「打完一枪就退休」。
 *
 * 两个回调都打印触发时刻(把当前 tick 当毫秒看),并在前面标上自己的名字,
 * 这样输出里你能一眼分清「这条是周期定时器每半秒响一次」「那条是单次定时器
 * 第 2000ms 响了那一下、之后再也没响」。单次 vs 周期的区别,光看文字定义很
 * 抽象,跑出来一行行对一下时刻就立刻懂了。
 *
 * 但本章真正要带走的是另一件事,也是新手栽得最狠的坑:**定时器回调不是普通
 * 任务函数,它跑在「定时器服务任务」(timer service task,那个名字叫 "Tmr Svc"、
 * 优先级被模板设成 configMAX_PRIORITIES-1 的内核自动任务)的上下文里**。这意味
 * 着两件事:第一,所有定时器回调——不管你建了多少个定时器——都串行地挤在
 * 这同一个任务里依次执行,一个回调没返回,后面的定时器就只能排队干等;第二,
 * 你在回调里**绝不能阻塞**(不能 vTaskDelay、不能在队列上死等、不能拿互斥量
 * 长时间霸占),也**不能干长活**。回调的正确姿势是「立刻做完一件极短的事
 * (比如往队列里投一帧、给任务发个通知),然后立刻返回」,把真正的耗时工作
 * 丢给普通任务去做。一旦你在回调里 vTaskDelay 个几百毫秒,你会把整个定时器
 * 服务任务卡住,所有别的定时器全部集体迟到——这就是把回调当任务写的代价。
 *
 * 回调拿不到定时器该用的参数,FreeRTOS 的办法是把「每触发一次就单调 +1 的
 * 计数值」当参数传进来(回调签名是 void(*)(TimerHandle_t),那个 TimerHandle_t
 * 其实就是定时器自己的句柄,你能在回调里分辨「现在是我哪个定时器响了」,
 * 靠的就是它)。本 demo 里两个回调各自只认自己的定时器,直接打印即可。
 *
 * 「定时器命令是怎么跑到服务任务那里的」也值得提前知道:你调 xTimerStart 这类
 * API 时,它并不直接执行定时器逻辑,而是往一条「定时器命令队列」里塞一条
 * 命令(「请帮我启动这个定时器」),然后唤醒服务任务;服务任务从队列里取出
 * 命令、真正去操作定时器链表、到期时回调你的回调。所以软件定时器的精度受限于
 * 「服务任务被调度的及时性」和「命令队列长度」,它不是硬件中断,host 模拟下
 * 更谈不上实时——这一点教学边界章里讲过,别拿它当硬实时的兜底。
 *
 * 所有 FreeRTOS 必需的回调样板(vAssertCalled / 各类 hook / 静态分配内存)
 * 都在 app_hooks.c 里,这个文件只放 demo 本身的逻辑。CMakeLists 已把
 * FREERTOS_HEAP 选成 4、configTOTAL_HEAP_SIZE 抬到 2MB,和 03/04/05 一致:
 * POSIX port 的任务栈一个个都有 128KB,固定大小的堆必须给足,而且我们要能
 * 读 xPortGetFreeHeapSize 系列函数。
 */

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "console.h"

/* 两个定时器的周期。单位是「tick」,下面用 pdMS_TO_TICKS 把毫秒换算成 tick。
 * tick 频率是 1000Hz(configTICK_RATE_HZ),所以 1 tick = 1ms,host 模拟下
 * 换算系数恰好是 1:1;真硬件上 tick 频率常是 100Hz,这个换算系数会变,
 * 这也是为什么 demo 里到处用 pdMS_TO_TICKS 而不是直接写毫秒数。 */
#define PERIODIC_TIMER_PERIOD_MS    ( 500 )   /* 周期定时器:每 500ms 响一次 */
#define ONESHOT_TIMER_PERIOD_MS     ( 2000 )  /* 单次定时器:2000ms 后响唯一一次 */

/* 工具:把当前 tick 当「从启动起多少毫秒」前缀打印出来。tick 频率 1000Hz,
 * 所以 xTaskGetTickCount() 直接就是毫秒数。注意这个函数既能在任务里调,
 * 也能在定时器回调里调(回调本身就跑在服务任务上下文,是合法的任务上下文),
 * 不属于「中断上下文」那种受限制的 FromISR 世界。 */
static void prvPrintTickPrefix( const char *pcTag )
{
    console_print( "[%5lu ms] %s ",
                   ( unsigned long ) xTaskGetTickCount(),
                   pcTag );
}

/* 周期定时器回调:每 PERIODIC_TIMER_PERIOD_MS 毫秒被服务任务调一次。
 * 它做的就是「打印此刻」,让你在输出里看到一条等间隔滚动的线。
 * 注意它必须在「极短时间」内返回——这里只做一次 console_print,完全合规。
 * 如果换成「回调里 vTaskDelay(500) 假装在干活」,你就会看到单次定时器的触发
 * 也跟着迟到,因为服务任务被这个回调卡住了。 */
static void prvPeriodicTimerCallback( TimerHandle_t xTimer )
{
    ( void ) xTimer;  /* 单一定时器共用本回调,不需要靠句柄区分,但签名要求带上 */
    prvPrintTickPrefix( "periodic timer fired  ->" );
    console_print( " (repeats every %d ms)\n", PERIODIC_TIMER_PERIOD_MS );
}

/* 单次定时器回调:它只在启动 ONESHOT_TIMER_PERIOD_MS 毫秒后响唯一一次,
 * 之后再也不响(除非你重新 xTimerStart 它)。回调里我们打印「我只响这一下」,
 * 并特意报出从启动到现在过了多少 tick,验证它确实是在 ~2000ms 处响的。 */
static void prvOneShotTimerCallback( TimerHandle_t xTimer )
{
    ( void ) xTimer;
    prvPrintTickPrefix( "one-shot timer fired ->" );
    console_print( " (fires ONCE after %d ms, then retires)\n",
                   ONESHOT_TIMER_PERIOD_MS );
}

int main( void )
{
    /* 关掉 stdout 全缓冲,避免重定向/管道时输出丢失(见 02_environment 排坑章)。 */
    setvbuf( stdout, NULL, _IOLBF, 0 );

    console_init();

    /* xTimerCreate 的六个参数,逐个交代:
     *   1) 名字("Periodic"/"OneShot"):调试用,vTaskList/uxTaskGetSystemState
     *      里会显示它,长度上限是 configMAX_TASK_NAME_LEN(模板里 12)。
     *   2) 周期:单位是 tick。周期定时器填的就是「两次触发之间的间隔」;
     *      单次定时器填的是「从启动到第一次(也是唯一一次)触发的延迟」。
     *      这里都用 pdMS_TO_TICKS 把毫秒换算成 tick。
     *   3) uxAutoReload:pdTRUE = 周期定时器(到期后自动重装、周而复始);
     *      pdFALSE = 单次定时器(到期一次就停)。这一位就是「周期 vs 单次」
     *      的全部区别所在。
     *   4) pvTimerID:回调里能拿到的「身份标识」,多定时器共用一个回调时用它
     *      区分是谁响了。本 demo 两个定时器各用各的回调,这里传 NULL 即可。
     *   5) 回调函数指针:定时器到期时、服务任务上下文里被调用的那个函数。
     * 返回值是 TimerHandle_t,创建失败(heap 不够)返回 NULL——致命错误直接断言。
     */
    TimerHandle_t xPeriodicTimer = xTimerCreate(
        "Periodic",
        pdMS_TO_TICKS( PERIODIC_TIMER_PERIOD_MS ),
        pdTRUE,                          /* uxAutoReload=pdTRUE -> 周期 */
        NULL,
        prvPeriodicTimerCallback );
    configASSERT( xPeriodicTimer != NULL );

    TimerHandle_t xOneShotTimer = xTimerCreate(
        "OneShot",
        pdMS_TO_TICKS( ONESHOT_TIMER_PERIOD_MS ),
        pdFALSE,                         /* uxAutoReload=pdFALSE -> 单次 */
        NULL,
        prvOneShotTimerCallback );
    configASSERT( xOneShotTimer != NULL );

    /* xTimerStart 把「请启动这个定时器」这条命令投进定时器命令队列。
     * 它的第二个参数是「如果命令队列满了,最多等多久再放弃」——这里给
     * portMAX_DELAY 表示死等也要把命令塞进去。注意:xTimerStart 只是「投命令」,
     * 真正把定时器挂上链表、开始计时,是服务任务从队列里取出这条命令之后的事。
     * 所以严格说,定时器「开始计时」的时刻略晚于 xTimerStart 返回的时刻,
     * 这就是「软件定时器精度受服务任务调度及时性限制」的来源之一。
     * 这里两个 xTimerStart 都在 vTaskStartScheduler 之前调用——这是允许的,
     * 内核会在服务任务起来后(见 app_hooks.c 的 vApplicationDaemonTaskStartupHook
     * 那个时机附近)依次处理这些早投的命令。 */
    BaseType_t xOk1 = xTimerStart( xPeriodicTimer, portMAX_DELAY );
    BaseType_t xOk2 = xTimerStart( xOneShotTimer, portMAX_DELAY );
    configASSERT( xOk1 == pdPASS );
    configASSERT( xOk2 == pdPASS );

    console_print( "06_timers: periodic=%dms (auto-reload), one-shot=%dms (single fire)\n",
                   PERIODIC_TIMER_PERIOD_MS, ONESHOT_TIMER_PERIOD_MS );
    console_print( "06_timers: starting scheduler\n" );

    vTaskStartScheduler();

    /* 调度器正常情况下不会返回;走到这里说明堆不够建 idle/timer 任务,属致命错误。 */
    for( ; ; )
    {
    }

    return 0;
}
