/*
 * 12_debugging —— 调试章独立 demo。
 *
 * 目标是把调试章要教的几件「观测工具」亲手跑一遍,让读者在输出里看到它们的真实长相:
 *   (a) configASSERT —— 故意制造一个断言失败条件,看 vAssertCalled 打印的
 *       "ASSERT FAILED: <file>:<line>" 长什么样。这是 FreeRTOS 开发阶段最重要的「快速失败」
 *       机制:你写错 API 用法(传 NULL 句柄、越界优先级……)时,内核会当场断言、停下,
 *       把出错文件名+行号甩给你,而不是让一个静默的 bug 潜伏到生产环境。
 *   (b) uxTaskGetSystemState / vTaskList —— 把系统里所有任务的「状态快照」拍出来打印
 *       (任务名 / 状态机 / 优先级 / 栈高水位)。这是 04_tasks 章已经演过的「打印」路线,
 *       本章顺带把 vTaskList(它受 configUSE_STATS_FORMATTING_FUNCTIONS 控制)那条路也走通。
 *   (c) configGENERATE_RUN_TIME_STATS —— 各任务的 CPU 占用百分比。这需要配一个
 *       「运行时统计时钟」,POSIX port 已经替我们接好了(ulPortGetRunTime),我们只要
 *       开 configGENERATE_RUN_TIME_STATS(模板已开=1)就能从 ulRunTimeCounter 算出占比。
 *
 * ⚠️ 诚实交代边界(文档专节展开):configCHECK_FOR_STACK_OVERFLOW 在 POSIX port 下
 * 实际不生效。真 MCU 上内核会在任务切换时给栈涂「魔数」、检查有没有被踩;但 POSIX port
 * 把每个任务映射成一个 pthread,pthread 栈由宿主 glibc 管,内核够不着——所以那个钩子
 * 在 host 模拟下永远不会被触发。我们 demo 里仍保留 vApplicationStackOverflowHook(见
 * app_hooks.c),它在真硬件上才有意义;host 下我们只能靠 uxTaskGetStackHighWaterMark
 * 「主动探」栈水位,不能靠内核「被动报警」。
 *
 * 实验组织方式:几个长周期任务各跑各的(模拟一个有负载的系统),一个优先级最低的
 * DebugTask 周期性地拍快照 + 打印运行时统计。assert 实验(实验 a)故意放在一个「单次」
 * 任务里,跑一下就触发、把 vAssertCalled 的输出露出来,然后用一个宏 ASKERT_DEMO_ENABLED
 * 开关它(默认关),这样日常 build/run 能看到完整的 (b)(c) 输出,要演 (a) 再开。
 *
 * 所有 FreeRTOS 必需的回调样板(vAssertCalled / 各类 hook / 静态分配内存)都在
 * app_hooks.c 里,这个文件只放 demo 本身的逻辑。CMakeLists 已选 heap_4 + 2MB(见 spec)。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "console.h"

/* 实验组织用的几个优先级。configMAX_PRIORITIES=7,合法 0..6。
 * DebugTask 压在最低档 +1(只在有空时跑),拍快照不能和被观测对象抢 CPU;
 * 负载任务给中高优先级,确保有真实的 CPU 占用可统计。 */
#define prioWORKER_A    ( tskIDLE_PRIORITY + 3 )
#define prioWORKER_B    ( tskIDLE_PRIORITY + 2 )
#define prioIDLE_WORK    ( tskIDLE_PRIORITY + 1 )   /* 故意忙等的任务,抢 idle 时间,演示它有占用 */
#define prioDEBUG        ( tskIDLE_PRIORITY + 1 )

/* DebugTask 拍快照 + 打印运行时统计的周期。 */
#define SNAPSHOT_PERIOD_MS    ( 1000 )

/* 实验开关:是否在跑完启动序列后,用一个单次任务故意触发一次 configASSERT。
 * 默认关——开了之后 demo 会在 vAssertCalled 里陷入「while 变量==0 死循环」停下
 * (见 app_hooks.c 的 vAssertCalled),这正是「断言炸了就别往下走」的现场。
 * 开它演示完 (a) 之后,记得改回 0 再 build 看 (b)(c) 的连续输出。 */
#define ASSERT_DEMO_ENABLED    ( 0 )

/* 任务名长度上限是 configMAX_TASK_NAME_LEN(模板=12)。快照表对齐按 12 来排,
 * 和 vTaskList 的内部列宽一致。 */
#define TASK_NAME_WIDTH    ( configMAX_TASK_NAME_LEN )

/* 配合 uxTaskGetSystemState 的任务状态数组容量。host 下任务数很少(几个应用任务 +
 * idle + timer daemon),给 16 绰绰有余。 */
#define MAX_TASKS_IN_SNAPSHOT    ( 16 )

/* eTaskState 是个枚举,值是数字,打印时翻译成人话更好读。 */
static const char *prvStateName( eTaskState eState )
{
    switch( eState )
    {
        case eRunning:   return "Running";
        case eReady:     return "Ready";
        case eBlocked:   return "Blocked";
        case eSuspended: return "Suspend";
        case eDeleted:   return "Deleted";
        default:         return "?";
    }
}

/* 三个负载任务里有两个走「干一段活、睡一段」的标准节奏(用 vTaskDelay 进 blocked),
 * 它们各自有不同的忙等/睡眠比,所以在运行时统计里会呈现不同的 CPU 占用,正好演示
 * 「运行时统计能区分谁更吃 CPU」。 */
static void prvPeriodicWorker( void *pvParameters )
{
    uint32_t ulWorkTicks = ( uint32_t ) ( uintptr_t ) pvParameters;

    for( ; ; )
    {
        /* 忙等一段 tick:纯空转占用 CPU,模拟「在干活」。这是 host 模拟下「真实吃 CPU」
         * 的写法(纯忙等),所以运行时统计会把这段时间记成它的占用。 */
        TickType_t xStart = xTaskGetTickCount();
        while( ( xTaskGetTickCount() - xStart ) < ulWorkTicks )
        {
        }

        /* 睡一段(进 blocked,把 CPU 让给别人),让多任务得以并存。 */
        vTaskDelay( pdMS_TO_TICKS( 200 ) );
    }
}

/* 一个「半忙等」任务:它不靠 vTaskDelay 让出,而是忙等里频繁空转。这会让它在运行时
 * 统计里占一块醒目的百分比(因为它几乎一直在 ready/running),也顺便演示「idle 被谁
 * 吃掉了」——它的占用 + idle 的占用 + 别人的占用 加起来应接近 100%。 */
static void prvIdleBurnerTask( void *pvParameters )
{
    ( void ) pvParameters;

    for( ; ; )
    {
        /* 极短的忙等 + 极短的让出,循环往复。prvIdleBurner 会「抢走」大量本该归 idle
         * 的 CPU 时间,运行时统计里它的百分比会明显高于那两个周期 Worker。 */
        for( volatile uint32_t i = 0; i < 200000; i++ )
        {
        }
        taskYIELD();
    }
}

/* 实验开关开启时跑的一次性任务:启动后稍等片刻(让别的任务先就位、让快照能拍到正常态),
 * 然后故意制造一个断言失败条件,触发 vAssertCalled。
 *
 * 这里故意触发的是「把一个任务的优先级设成非法值 configMAX_PRIORITIES」——内核的
 * configASSERT 会拦下这种越界,当场调 vAssertCalled 把文件名+行号甩出来。这是教学里
 * 最安全的「演示断言」方式:不需要真损坏什么,内核就是设计成「越界优先级即断言」的。
 * 但更常见的是 configASSERT 拦截 NULL 句柄、ISR 里调错 API 等用法错误,机制完全一样。 */
#if ( ASSERT_DEMO_ENABLED == 1 )
static void prvAssertTriggerTask( void *pvParameters )
{
    ( void ) pvParameters;

    console_print( "assert-demo: waiting 800 ms before triggering configASSERT...\n" );
    vTaskDelay( pdMS_TO_TICKS( 800 ) );

    console_print( "assert-demo: deliberately setting priority to illegal value %d (max=%d)\n",
                   ( int ) configMAX_PRIORITIES,
                   ( int ) configMAX_PRIORITIES );

    /* vTaskPrioritySet 内部第一行就是 configASSERT(优先级 < configMAX_PRIORITIES)。
     * 传 configMAX_PRIORITIES(恰好越界 1),断言立即触发 → vAssertCalled 打印
     * "ASSERT FAILED: tasks.c:<line>" → 在 app_hooks.c 的 while 循环里停下。
     * 任务把自己当目标(传 NULL=当前任务),这样不需要存任何句柄。 */
    vTaskPrioritySet( NULL, configMAX_PRIORITIES );

    /* 正常情况下到不了这里(上面那行就断言炸了)。如果 ASSERT 没开(比如 FreeRTOSConfig
     * 走的是 coverage 配置把 configASSERT 关了),这行才会被执行,我们自删收尾。 */
    console_print( "assert-demo: ASSERT did NOT fire (configASSERT disabled?), self-deleting\n" );
    vTaskDelete( NULL );
}
#endif /* ASSERT_DEMO_ENABLED */

/* DebugTask:周期性做三件事——(1) 用 uxTaskGetSystemState 拍「状态快照」打印(实验 b);
 * (2) 用 vTaskList 打印一份等价但格式固定的表(也属实验 b,展示另一条路);
 * (3) 在快照里带上 ulRunTimeCounter,折算成 CPU 占用百分比(实验 c)。
 * 它优先级压到最低,只在「没有正经任务想跑」时插进来,否则它自己会污染统计。 */
static void prvDebugTask( void *pvParameters )
{
    ( void ) pvParameters;
    static TaskStatus_t xStatus[ MAX_TASKS_IN_SNAPSHOT ];

    /* 启动后先睡一拍,让负载任务先跑起来、积累一些运行时统计,否则第一张快照全是 0%。
     * uxTaskGetSystemState 返回的是「自调度器启动以来的累计运行时」,睡 1 秒让数字
     * 有意义。 */
    vTaskDelay( pdMS_TO_TICKS( SNAPSHOT_PERIOD_MS ) );

    for( ; ; )
    {
        console_print( "============ debug snapshot @ tick %lu ============\n",
                       ( unsigned long ) xTaskGetTickCount() );

        /* 实验中心 (b)+(c):uxTaskGetSystemState 一次性把所有任务的状态填进数组,
         * 同时(因为开了 configGENERATE_RUN_TIME_STATS)把累计运行时填进 *pulTotalRunTime。
         * 返回值是实际填了多少条。 */
        configRUN_TIME_COUNTER_TYPE ulTotalRunTime = 0;
        UBaseType_t uxCount = uxTaskGetSystemState( xStatus, MAX_TASKS_IN_SNAPSHOT, &ulTotalRunTime );

        if( uxCount == 0 || ulTotalRunTime == 0 )
        {
            console_print( "  (no tasks / no run-time stats yet)\n" );
        }
        else
        {
            /* 先按运行时占用降序排一下,让最吃 CPU 的任务排在最上面,读起来直观。
             * 任务数很少,O(n^2) 选择排序无所谓。 */
            for( UBaseType_t i = 0; i + 1 < uxCount; i++ )
            {
                for( UBaseType_t j = i + 1; j < uxCount; j++ )
                {
                    if( xStatus[ j ].ulRunTimeCounter > xStatus[ i ].ulRunTimeCounter )
                    {
                        TaskStatus_t xTmp = xStatus[ i ];
                        xStatus[ i ] = xStatus[ j ];
                        xStatus[ j ] = xTmp;
                    }
                }
            }

            /* 表头 + 每个任务一行:名字 / 状态 / 优先级 / 栈高水位(字) / CPU 占用 %。
             * 占用 % = 该任务 ulRunTimeCounter × 100 / 总运行时。占用不足 1% 的标 <1%。
             * 栈高水位单位是「字(StackType_t)」,不是字节;host 下都虚胖到 16k+,真 MCU
             * 上这个数才是排查「栈够不够」的关键水位。 */
            console_print( "  %-12s %-8s prio  freestack   CPU%%\n", "name", "state" );
            for( UBaseType_t i = 0; i < uxCount; i++ )
            {
                /* ulRunTimeCounter 是 configRUN_TIME_COUNTER_TYPE(默认 uint32_t)。算百分比
                 * 用 (counter*100)/total 避免浮点;小于 1% 的标 <1%。把百分比折成一段短串
                 * (要么 "NN%" 要么 "<1%")塞进同一行,读起来是一张整齐的表。 */
                uint32_t ulPct = ( uint32_t ) ( ( xStatus[ i ].ulRunTimeCounter * 100UL ) / ulTotalRunTime );
                char cPct[ 16 ];
                if( ulPct == 0 )
                {
                    snprintf( cPct, sizeof( cPct ), "<1%%" );
                }
                else
                {
                    snprintf( cPct, sizeof( cPct ), "%lu%%", ( unsigned long ) ulPct );
                }
                console_print( "  %-12s %-8s  %2lu   %8lu   %s\n",
                               xStatus[ i ].pcTaskName,
                               prvStateName( xStatus[ i ].eCurrentState ),
                               ( unsigned long ) xStatus[ i ].uxCurrentPriority,
                               ( unsigned long ) xStatus[ i ].usStackHighWaterMark,
                               cPct );
            }
        }

        /* 实验另一条路:vTaskList。它和 uxTaskGetSystemState 看的是同一份数据,但把格式化
         * 打印封装好了。模板的 configUSE_STATS_FORMATTING_FUNCTIONS=0 把 vTaskList/vTaskGetRunTimeStats
         * 关了——我们在 FreeRTOSConfig.h 没动这个开关的前提下,这条路在 demo 里就拿不到。
         * 所以这里做个「能探测到就跑、探测不到就说明为什么」的演示,而不是去改配置开关
         * (样板尽量不动,改了反而要让读者翻 FreeRTOSConfig 找)。我们用 uxTaskGetSystemState
         * 自己格式化(上面那段)才是 spec 推荐的教学路线。
         *   vTaskList 的列格式是固定的:Name  State  Priority  FreeStack。
         *   host 下若 configUSE_STATS_FORMATTING_FUNCTIONS=0,链接器会把 vTaskList 整个
         *   剔掉,所以这里我们干脆不调它,文档里点明「想要它就在 FreeRTOSConfig 把开关置 1」。 */
        console_print( "  (vTaskList is %s; set configUSE_STATS_FORMATTING_FUNCTIONS=1 to enable)\n",
#if ( configUSE_STATS_FORMATTING_FUNCTIONS == 1 )
                       "enabled"
#else
                       "disabled"
#endif
                       );

        vTaskDelay( pdMS_TO_TICKS( SNAPSHOT_PERIOD_MS ) );
    }
}

int main( void )
{
    /* 关掉 stdout 全缓冲,避免重定向/管道时输出丢失(见 02_environment 排坑章)。 */
    setvbuf( stdout, NULL, _IOLBF, 0 );

    console_init();

    /* 两个周期负载任务:忙等/睡眠比不同 → 运行时统计里 CPU 占用不同。 */
    xTaskCreate( prvPeriodicWorker, "WorkerA", configMINIMAL_STACK_SIZE,
                 ( void * ) ( uintptr_t ) 20, prioWORKER_A, NULL );
    xTaskCreate( prvPeriodicWorker, "WorkerB", configMINIMAL_STACK_SIZE,
                 ( void * ) ( uintptr_t ) 10, prioWORKER_B, NULL );

    /* 半忙等任务:抢 idle 时间,演示「它把 idle 的占用吃掉了」。 */
    xTaskCreate( prvIdleBurnerTask, "IdleBurn", configMINIMAL_STACK_SIZE,
                 NULL, prioIDLE_WORK, NULL );

    /* DebugTask:拍快照 + 打印运行时统计。优先级最低。 */
    xTaskCreate( prvDebugTask, "Debug", configMINIMAL_STACK_SIZE,
                 NULL, prioDEBUG, NULL );

#if ( ASSERT_DEMO_ENABLED == 1 )
    /* 实验 (a):单次任务,延时后故意触发 configASSERT。默认关(见文件头注释)。 */
    xTaskCreate( prvAssertTriggerTask, "AssertTrig", configMINIMAL_STACK_SIZE,
                 NULL, prioWORKER_A, NULL );
#endif

    console_print( "12_debugging: starting scheduler (heap=%lu bytes, max_prio=%d, ASSERT_DEMO=%d)\n",
                   ( unsigned long ) configTOTAL_HEAP_SIZE,
                   ( int ) configMAX_PRIORITIES,
                   ( int ) ASSERT_DEMO_ENABLED );

    vTaskStartScheduler();

    for( ; ; )
    {
    }

    return 0;
}
