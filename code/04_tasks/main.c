/*
 * 04_tasks —— 任务管理 demo。
 *
 * 目标:创建一组不同优先级的任务,亲手观察调度器是怎么在它们之间挑选和切换的,
 * 并把每个任务的状态(ready / running / blocked)拍下来。这一章正式进入
 * xTaskCreate,把上一章「任务的栈和 TCB 都从堆里来」的结论落到可运行的东西上。
 *
 * 这个 demo 安排了三类任务:
 *   - 三个 Worker 任务(High / Mid / Low),优先级依次降低,各自周期性干活并
 *     在干活时打印自己「此刻在 running」。它们的 vTaskDelay 时间故意设得不同,
 *     好让调度器在不同时刻总能挑出不同优先级的 ready 任务来跑,调度轨迹才看得清。
 *   - 一个 Monitor 任务,优先级最低(只比 idle 高一点点),周期性把所有任务的
 *     状态拍一张快照打印出来——这是「在系统外部观察系统内部」的窗口,你能看到
 *     每个 Worker 此刻是 running、ready 还是 blocked,调度器的工作一览无余。
 *
 * 为什么用 uxTaskGetSystemState 而不是 vTaskList:vTaskList 受
 * configUSE_STATS_FORMATTING_FUNCTIONS 控制,模板里它是关着的(=0);而
 * uxTaskGetSystemState 只要 configUSE_TRACE_FACILITY=1 就可用(模板已开)。
 * 它返回的是结构体数组,我们自己格式化打印,既绕开了那个开关,输出也比
 * vTaskList 的固定格式更可控、更适合教学。
 *
 * 所有 FreeRTOS 必需的回调样板在 app_hooks.c,这里只放 demo 逻辑。
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "console.h"

/* 三个 Worker 的优先级。注意 configMAX_PRIORITIES=7,合法优先级是 0..6,
 * idle 任务固定占 0。我们让 High/Mid/Low 依次为 4/3/2,留出明显梯度。 */
#define prioWORKER_HIGH     ( tskIDLE_PRIORITY + 4 )
#define prioWORKER_MID      ( tskIDLE_PRIORITY + 3 )
#define prioWORKER_LOW      ( tskIDLE_PRIORITY + 2 )
/* Monitor 优先级压到最低,确保它只在「没有正经任务想跑」时才插进来,
 * 这样它拍的快照才不会被自己干扰——它不能和 Worker 抢 CPU。 */
#define prioMONITOR         ( tskIDLE_PRIORITY + 1 )

/* Worker 干活的参数:名字 + 每轮 active 的时长 + 每轮 sleep 的时长。
 * 不同的 sleep 周期让三个任务在不同时刻各自进入 blocked,调度器才有机会
 * 在它们之间切换,调度轨迹才丰富。 */
typedef struct
{
    const char *pcName;
    TickType_t  xActiveTicks;   /* 模拟「干活」的忙等时长 */
    TickType_t  xSleepTicks;    /* 然后 vTaskDelay 这么久,进入 blocked */
} WorkerConfig_t;

/* 忙等 helper:纯粹消耗 xActiveTicks 个 tick 来模拟「占用 CPU 干活」。
 * 我们用一个 volatile 累加阻止编译器优化掉它。在 preemptive 调度下,
 * 高优先级任务忙等期间会独占 CPU,低优先级任务根本得不到运行——这恰好
 * 是我们要演示的「优先级调度」的本质。 */
static void prvBurnCycles( TickType_t xTicks )
{
    volatile unsigned long ulJunk = 0;
    TickType_t xStart = xTaskGetTickCount();

    while( ( xTaskGetTickCount() - xStart ) < xTicks )
    {
        ulJunk += 1;
    }

    ( void ) ulJunk;
}

/* Worker 任务:每轮先忙等 active 时长(占用 CPU),再 vTaskDelay sleep 时长
 * (让出 CPU、进入 blocked),循环往复。每次进入 running 和进入 blocked
 * 都打一行,这样把调度器「选谁、谁让出」的决策链拼出来。 */
static void prvWorkerTask( void *pvParameters )
{
    WorkerConfig_t *pxCfg = ( WorkerConfig_t * ) pvParameters;

    for( ; ; )
    {
        console_print( "[%s] running, working %lu ticks...\n",
                       pxCfg->pcName,
                       ( unsigned long ) pxCfg->xActiveTicks );

        prvBurnCycles( pxCfg->xActiveTicks );

        console_print( "[%s] work done, sleeping %lu ticks (blocked)\n",
                       pxCfg->pcName,
                       ( unsigned long ) pxCfg->xSleepTicks );

        /* vTaskDelay 把自己挂到 blocked 直到超时;这段时间 CPU 让给别人。
         * 这就是「任务主动让出」和「任务被抢占」的区别:vTaskDelay 是主动的。 */
        vTaskDelay( pxCfg->xSleepTicks );
    }
}

/* 把 eTaskState 枚举翻成人话,快照里好看。 */
static const char *pcStateName( eTaskState eState )
{
    switch( eState )
    {
        case eRunning:   return "Running";
        case eReady:     return "Ready";
        case eBlocked:   return "Blocked";
        case eSuspended: return "Suspended";
        case eDeleted:   return "Deleted";
        default:         return "?";
    }
}

/* Monitor 任务:周期性把所有任务的状态拍一张快照。
 * uxTaskGetSystemState 一次性把系统里所有任务(含 idle/timer)的快照填进数组,
 * 返回填了多少条。我们自己按 优先级降序 排一下再打,这样高优先级在上,
 * 和我们对「调度器优先选高优先级」的直觉一致。每轮之间也 vTaskDelay,
 * 让 Monitor 自己别把 CPU 占满。 */
static void prvMonitorTask( void *pvParameters )
{
    ( void ) pvParameters;
    /* 任务数上限取一个稳妥值;真正条数用 uxTaskGetSystemState 的返回值。 */
    const UBaseType_t uxMaxTasks = 8;
    static TaskStatus_t pxStatus[ 8 ];

    for( ; ; )
    {
        vTaskDelay( pdMS_TO_TICKS( 800 ) );

        UBaseType_t uxCount = uxTaskGetSystemState( pxStatus, uxMaxTasks, NULL );

        /* 简单选择排序:按优先级降序。任务数很少(几个),O(n^2) 完全无所谓。 */
        for( UBaseType_t i = 0; i + 1 < uxCount; i++ )
        {
            for( UBaseType_t j = i + 1; j < uxCount; j++ )
            {
                if( pxStatus[ j ].uxCurrentPriority > pxStatus[ i ].uxCurrentPriority )
                {
                    TaskStatus_t xTmp = pxStatus[ i ];
                    pxStatus[ i ] = pxStatus[ j ];
                    pxStatus[ j ] = xTmp;
                }
            }
        }

        console_print( "---- task snapshot (prio desc) ----\n" );
        for( UBaseType_t i = 0; i < uxCount; i++ )
        {
            console_print( "  %-12s state=%-9s prio=%lu freestack=%lu\n",
                           pxStatus[ i ].pcTaskName,
                           pcStateName( pxStatus[ i ].eCurrentState ),
                           ( unsigned long ) pxStatus[ i ].uxCurrentPriority,
                           ( unsigned long ) pxStatus[ i ].usStackHighWaterMark );
        }
        console_print( "-----------------------------------\n" );
    }
}

int main( void )
{
    /* 关掉 stdout 全缓冲,避免重定向/管道时输出丢失(见 02_environment 排坑章)。 */
    setvbuf( stdout, NULL, _IOLBF, 0 );

    console_init();

    /* 三个 Worker 的配置。sleep 周期拉开差异,调度轨迹才丰富。 */
    static WorkerConfig_t xCfgHigh = { "WorkerHigh", 50,  400 };
    static WorkerConfig_t xCfgMid  = { "WorkerMid",  40,  700 };
    static WorkerConfig_t xCfgLow  = { "WorkerLow",  30, 1000 };

    xTaskCreate( prvWorkerTask, "WorkerHigh", configMINIMAL_STACK_SIZE, &xCfgHigh,
                 prioWORKER_HIGH, NULL );
    xTaskCreate( prvWorkerTask, "WorkerMid",  configMINIMAL_STACK_SIZE, &xCfgMid,
                 prioWORKER_MID, NULL );
    xTaskCreate( prvWorkerTask, "WorkerLow",  configMINIMAL_STACK_SIZE, &xCfgLow,
                 prioWORKER_LOW, NULL );
    xTaskCreate( prvMonitorTask, "Monitor", configMINIMAL_STACK_SIZE, NULL,
                 prioMONITOR, NULL );

    console_print( "04_tasks: starting scheduler (configMAX_PRIORITIES=%d)\n",
                   configMAX_PRIORITIES );

    vTaskStartScheduler();

    for( ; ; )
    {
    }

    return 0;
}
