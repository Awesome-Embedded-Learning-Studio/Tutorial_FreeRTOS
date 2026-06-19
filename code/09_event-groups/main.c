/*
 * 09_event-groups —— 事件组(Event Group)演示。
 *
 * 事件组是 FreeRTOS 的「一组布尔标志位 + 一个可以等这些位组合的任务」机制。
 * 它解决的是「一个任务要等好几个互相独立的事件同时/分别到位才能动」这种
 * 多事件同步问题——这正是前几章的队列、信号量、任务通知都不擅长的事:
 *   - 队列传「数据」,信号量/任务通知传「一个事件」,它们都是「一对一」的;
 *   - 而事件组是「一组位」,任意多个任务都能往里 set 不同的位,任意一个任务都能
 *     等一个「这些位的组合」(全部到位=AND,或任意一个到位=OR)。
 *
 * 本 demo 的事件流(把事件组当成一块「公共状态板」):
 *   - 一块事件组 xBootEvents,定义三个位:
 *       BIT_NETWORK_READY (bit0):网络子系统就绪
 *       BIT_SENSOR_READY  (bit1):传感器子系统就绪
 *       BIT_LOG_READY     (bit2):日志子系统就绪
 *   - 三个独立的「子系统」任务(NetTask / SensorTask / LogTask)各自 vTaskDelay
 *     一段不同的时间模拟「异步初始化」,初始化「完成」时各自往事件组里 set 自己
 *     那一位。它们谁也不等谁、各跑各的——这就是事件组「多个 setter 并发置位」的样子。
 *   - 一个 CoordinatorTask 用 xEventGroupWaitBits 等「三个位全部到位」(AND 等待),
 *     到齐了就宣布「all systems go」——这就是「等多事件全部就绪」的组合等待。
 *
 * 等待的两种模式(本章的两个核心知识点)我们都演:
 *   - AND 等待:xWaitForAllBits = pdTRUE,要 uxBitsToWaitFor 里指定的位「全部」置位
 *     才返回。CoordinatorTask 的初始化同步就用它。
 *   - OR 等待:xWaitForAllBits = pdFALSE,uxBitsToWaitFor 里指定的位「任意一个」置位
 *     就返回。我们在 demo 第二阶段演一把 OR 等待 + 超时。
 *
 * 「清除」这件事也要专门讲:xEventGroupWaitBits 有个 xClearOnExit 参数,pdTRUE 时
 * 它会在「成功返回」那一刻把等到的那些位清掉(原子地「读到并清掉」,避免 set 和 clear
 * 的竞态);想要位持续保留(比如多个等待者都要看到),就传 pdFALSE、改用手动
 * xEventGroupClearBits。demo 里两种都演。
 *
 * 环境说明:POSIX host 模拟(heap_4 + 2MB,见 CMakeLists/FreeRTOSConfig.h,沿用
 * dashboard 已踩平的配置)。所有 FreeRTOS 必需的回调样板在 app_hooks.c,本文件
 * 只放 demo 逻辑。
 *
 * 教学边界(诚实交代):事件组本身是纯软件机制,host 模拟和真硬件行为完全一致;
 * set/wait 的同步语义、AND/OR、超时,这套东西不存在「host 模拟不真实」的问题。
 * 唯一在 host 下需要小心的是 xEventGroupSetBitsFromISR(中断上下文里 set 位)——
 * 它内部走定时器服务任务代发,和中断管理章讲的 FromISR 同理,本章 demo 全程在
 * 任务上下文里 set 位,不涉及 ISR,因此没有那个坑点。
 */

#include <stdbool.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"

#include "console.h"

/* 子系统初始化 / 协调任务的优先级。configMAX_PRIORITIES=7,合法 0..6。
 * 三个子系统初始化任务给 +1(初始化是后台活、不抢主线);协调任务给 +2,
 * 它要等齐才动,给高一点保证它一就绪就被及时调度。 */
#define prioSUBSYSTEM     ( tskIDLE_PRIORITY + 1 )
#define prioCOORDINATOR   ( tskIDLE_PRIORITY + 2 )

/* 三个子系统的「模拟初始化耗时」。故意错开(不同数量级),让它们「就绪」的
 * 时刻明显有先后,输出里能看清「最后那个到位的位,才让 AND 等待解除」。 */
#define NETWORK_INIT_MS    ( 300 )   /* 网络最快就绪 */
#define SENSOR_INIT_MS     ( 800 )   /* 传感器第二 */
#define LOG_INIT_MS        ( 1500 )  /* 日志最慢——它到位前 Coordinator 一直等 */

/* 事件组里定义的三个位。每个位代表「某个子系统就绪」这个布尔事实。
 * 用 ( 1 << n ) 是事件组位的标准写法;EventBits_t 是 24 位可用(configUSE_16_BIT_TICKS=0
 * 时 8 位控制位 + 24 位事件位),这里只用到 3 位,绰绰有余。 */
#define BIT_NETWORK_READY    ( 1 << 0 )
#define BIT_SENSOR_READY     ( 1 << 1 )
#define BIT_LOG_READY        ( 1 << 2 )
/* 「全部就绪」的掩码 = 三个位的或,AND 等待时传它。 */
#define BITS_ALL_READY       ( BIT_NETWORK_READY | BIT_SENSOR_READY | BIT_LOG_READY )

/* 事件组句柄。进 scheduler 之前 xEventGroupCreate 创建好。 */
static EventGroupHandle_t xBootEvents = NULL;

/* NetTask:模拟网络子系统初始化。延时 NETWORK_INIT_MS 后,往事件组 set BIT_NETWORK_READY。
 * set 这一下如果恰好有任务在 xEventGroupWaitBits 上等这个位,会被内核自动唤醒——
 * setter 不用知道「谁在等」,这正是事件组「广播」式的解耦:set 一次,所有在等的
 * 任务内核会逐一检查、各按自己的等待条件决定要不要醒。 */
static void prvNetTask( void *pvParameters )
{
    ( void ) pvParameters;
    console_print( "[net]     init started\n" );
    vTaskDelay( pdMS_TO_TICKS( NETWORK_INIT_MS ) );
    console_print( "[net]     init done, set BIT_NETWORK_READY\n" );
    xEventGroupSetBits( xBootEvents, BIT_NETWORK_READY );
    /* set 完就退出:任务函数 return 等价于 vTaskDelete(NULL),这里显式自删。 */
    vTaskDelete( NULL );
}

/* SensorTask:模拟传感器子系统初始化,延时后 set BIT_SENSOR_READY。 */
static void prvSensorTask( void *pvParameters )
{
    ( void ) pvParameters;
    console_print( "[sensor]  init started\n" );
    vTaskDelay( pdMS_TO_TICKS( SENSOR_INIT_MS ) );
    console_print( "[sensor]  init done, set BIT_SENSOR_READY\n" );
    xEventGroupSetBits( xBootEvents, BIT_SENSOR_READY );
    vTaskDelete( NULL );
}

/* LogTask:模拟日志子系统初始化,延时后 set BIT_LOG_READY。
 * 它是三个里最慢的(LOG_INIT_MS=1500),所以 AND 等待的 Coordinator 会一直阻塞到它。 */
static void prvLogTask( void *pvParameters )
{
    ( void ) pvParameters;
    console_print( "[log]     init started\n" );
    vTaskDelay( pdMS_TO_TICKS( LOG_INIT_MS ) );
    console_print( "[log]     init done, set BIT_LOG_READY\n" );
    xEventGroupSetBits( xBootEvents, BIT_LOG_READY );
    vTaskDelete( NULL );
}

/* CoordinatorTask:演示事件组的两个核心知识点——AND 组合等待、清除语义、超时。
 *
 * 分两段演:
 *   第一段(AND 等待):等 BITS_ALL_READY「全部」到位(xWaitForAllBits = pdTRUE),
 *     到齐才宣布系统就绪。这正是「等一组初始化事件全部完成」的经典用法。
 *     xClearOnExit = pdTRUE:返回时把等到的位原子地清掉——这样「就绪过一次」这件事
 *     不会被长期留着,如果后面我们想复用这套位做第二轮,位是干净的。
 *   第二段(OR 等待 + 超时):故意先 set 一个位、用 OR 模式(任意一个就返回)+
 *     短超时,演示「事件没来时 WaitBits 怎么超时返回」、以及「按位返回值告诉我们
 *     哪些位是 set 的」。
 */
static void prvCoordinatorTask( void *pvParameters )
{
    ( void ) pvParameters;

    /* ===== 第一段:AND 等待「全部就绪」===== */
    console_print( "[coord]   waiting for ALL subsystems (AND) ...\n" );

    /* xEventGroupWaitBits 参数逐个解释:
     *   xBootEvents        —— 在哪个事件组上等;
     *   BITS_ALL_READY     —— uxBitsToWaitFor:我关心的位;
     *   xClearOnExit=pdTRUE—— 成功返回时把「等到的那些位」清掉(原子「读到并清」);
     *   xWaitForAllBits=pdTRUE —— AND 模式:要 uxBitsToWaitFor 全部位都置位才返回;
     *   portMAX_DELAY      —— 死等,不超时。
     * 返回值是「返回那一刻事件组的快照」,我们后面用它判断「到底哪些位到位」。 */
    EventBits_t xBits = xEventGroupWaitBits( xBootEvents,
                                             BITS_ALL_READY,
                                             pdTRUE,    /* 清除等到的位 */
                                             pdTRUE,    /* AND:全部到位才返回 */
                                             portMAX_DELAY );

    console_print( "[coord]   ALL READY (bits=0x%lx) -> all systems go!\n",
                   ( unsigned long ) xBits );

    /* 这一段过去后,因为 xClearOnExit=pdTRUE,三个就绪位已经被清回 0。我们用
     * xEventGroupGetBits 打印确认一下——你会看到 0x0,印证「ClearOnExit 把位清了」。
     * xEventGroupGetBits 是个只读快照,不清位。 */
    console_print( "[coord]   after clear-on-exit, group bits = 0x%lx\n",
                   ( unsigned long ) xEventGroupGetBits( xBootEvents ) );

    /* ===== 第二段:OR 等待 + 超时,演示「事件没来时怎么超时返回」===== */
    /* 现在位都被清空了。我们故意只 set 一个 BIT_NETWORK_READY(模拟「只有网络有动静」),
     * 然后用一个 OR 等待等「网络 OR 传感器」(任一即可),带 200ms 短超时。
     * 第一次等:因为网络位已经 set,OR 等待会立刻成功返回。
     * 第二次等:位已被 xClearOnExit 清掉、又没人再 set,所以这次会走满 200ms 超时,
     *          返回值告诉你是「超时」而不是「事件来了」——返回的位里不包含你等的位。
     * 这段把「OR 模式」和「超时返回」这两个常考点一次演清。 */
    console_print( "[coord]   set only BIT_NETWORK_READY, then OR-wait net|sensor\n" );
    xEventGroupSetBits( xBootEvents, BIT_NETWORK_READY );

    /* 第一次 OR 等待:位已就绪,立刻返回,返回值包含 BIT_NETWORK_READY。 */
    xBits = xEventGroupWaitBits( xBootEvents,
                                 BIT_NETWORK_READY | BIT_SENSOR_READY,
                                 pdTRUE,    /* 返回时清掉等到的位 */
                                 pdFALSE,   /* OR:任一就绪即返回 */
                                 pdMS_TO_TICKS( 200 ) );
    console_print( "[coord]   OR-wait returned bits=0x%lx (net ready, immediate)\n",
                   ( unsigned long ) xBits );

    /* 第二次 OR 等待:位已被清、无人再 set,200ms 后超时返回。
     * 关键判读:返回值里 BIT_NETWORK_READY|BIT_SENSOR_READY 都不在(都是 0),
     * 这正是「我是超时返回的、不是我等的事件来了」的判据。 */
    xBits = xEventGroupWaitBits( xBootEvents,
                                 BIT_NETWORK_READY | BIT_SENSOR_READY,
                                 pdTRUE,
                                 pdFALSE,
                                 pdMS_TO_TICKS( 200 ) );
    console_print( "[coord]   OR-wait timed out, bits=0x%lx (none of net|sensor set)\n",
                   ( unsigned long ) xBits );

    console_print( "[coord]   demo complete\n" );

    /* 任务退出:删除自己。删除后调度器只剩空闲任务,但本 demo 到此为止。 */
    vTaskDelete( NULL );
}

int main( void )
{
    /* 关掉 stdout 全缓冲,避免重定向/管道时输出丢失(见 02_environment 排坑章)。 */
    setvbuf( stdout, NULL, _IOLBF, 0 );

    console_init();

    /* 事件组必须在进 scheduler 之前创建好。xEventGroupCreate 动态分配一块
     * EventGroup_t(含那 24 个事件位 + 等待链表),返回句柄。 */
    xBootEvents = xEventGroupCreate();
    configASSERT( xBootEvents != NULL );

    /* 三个子系统初始化任务:各自 set 一个不同的位,谁也不等谁。 */
    xTaskCreate( prvNetTask, "Net", configMINIMAL_STACK_SIZE, NULL, prioSUBSYSTEM, NULL );
    xTaskCreate( prvSensorTask, "Sensor", configMINIMAL_STACK_SIZE, NULL, prioSUBSYSTEM, NULL );
    xTaskCreate( prvLogTask, "Log", configMINIMAL_STACK_SIZE, NULL, prioSUBSYSTEM, NULL );

    /* 协调任务:AND 等全部就绪,再演 OR + 超时。 */
    xTaskCreate( prvCoordinatorTask, "Coord", configMINIMAL_STACK_SIZE, NULL,
                 prioCOORDINATOR, NULL );

    console_print( "09_event-groups: boot-coordination demo (net=%d ms, sensor=%d ms, log=%d ms)\n",
                   NETWORK_INIT_MS, SENSOR_INIT_MS, LOG_INIT_MS );

    vTaskStartScheduler();

    /* 调度器正常情况下不会返回;走到这里说明堆不够建空闲任务(配置问题)。 */
    for( ; ; )
    {
    }

    return 0;
}
