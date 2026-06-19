/*
 * 08_resources —— 资源管理(互斥量)demo。
 *
 * 目标:亲手复现「优先级反转」,再亲眼看见 mutex 的「优先级继承」把它破解,
 * 两段输出摆在一起对比。这是资源管理这章唯一的核心,讲透了互斥量区别于二值信号量
 * 的根本——优先级继承。
 *
 * 经典反转剧本(三个任务 + 一把锁):
 *   - Low 任务(低优先级)先拿到共享资源的锁,然后开始干一段「慢活」(故意拖长,
 *     模拟它持有资源时还在磨磨蹭蹭)。
 *   - High 任务(高优先级)随后要拿同一把锁,发现锁在 Low 手里,自己 blocked 等锁。
 *   - Mid 任务(中等优先级)这时插进来——它不碰那把锁、纯粹是个长跑的计算任务,
 *     优先级比 Low 高,于是它一把抢过 CPU、独占磨蹭。Low 拿不到 CPU → 释放不了锁 →
 *     High 拿不到锁 → High 这个全系统最高优先级的任务,被一个完全无关的 Mid 任务
 *     「间接」饿死了。这就是优先级反转:不是被持锁者直接挡,是被一个八竿子打不着的中
 *     优先级任务挡,看起来极其反直觉。
 *
 * 破解:把锁从「二值信号量」换成「互斥量」。互斥量带优先级继承——High 在锁上 blocked
 * 的那一刻,内核会把持锁的 Low 临时抬到 High 的优先级,Mid 这下优先级不够、抢不动 Low,
 * Low 得以全速跑完、释放锁;Low 一 give,优先级降回去,High 立刻拿到锁、飞起来跑。
 * 反转消失。这就是 mutex 相对二值信号量唯一、也是决定性的区别。
 *
 * 用法:本 demo 用一个编译时开关 USE_MUTEX 选择用哪种锁。
 *   - 不定义 USE_MUTEX(grep 不见) → 用 xSemaphoreCreateBinary():二值信号量,
 *     无优先级继承 → 反转复现成功(High 被 Mid 间接饿死,严重延迟)。
 *   - 定义 USE_MUTEX → 用 xSemaphoreCreateMutex():互斥量,带优先级继承 → 反转被破解
 *     (High 几乎不延迟)。
 * 对应两段输出:本文档里贴的二值那段是「坏」的,互斥量那段是「好」的,摆一起对比。
 *
 * 时序设计要点(为什么这么排):
 *   - Low 在 t≈500ms 起跑、拿锁;High 在 t≈1000ms 起跑、撞锁;Mid 在 t≈1500ms 起跑、
 *     纯跑长活。三个起跑时刻错开,是要保证「Low 先持锁 → High 后撞锁 → Mid 再插队」
 *     这个因果顺序,反转才演得出来。Low 持锁期间故意拖到 t≈2500ms 才 give,期间留出
 *     足够窗口让 Mid 来插队、High 干等。
 *   - Mid 是个纯忙等的长任务(忙等约 3 秒),故意做成「既不让出 CPU、也不碰锁」——
 *     它就是那个把 Low 按在原地、间接饿死 High 的「搅局者」。在二值信号量下它会
 *     得逞;在互斥量下 Low 被抬优先级、它抢不动,原形毕露。
 *   - 每个关键节点(Low 拿锁、High 撞锁、Mid 插队、Low 放锁、High 拿到锁)都打一行带
 *     tick 的时间戳,这样两段输出摆一起,High 到底「等多久才拿到锁」一目了然——
 *     二值那段 High 会等到 Mid 跑完才解脱,延迟巨大;互斥量那段 High 几乎瞬间拿到。
 *
 * 所有 FreeRTOS 必需的回调样板在 app_hooks.c,这里只放 demo 逻辑。
 */

#include <stdbool.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "console.h"

/* 三个任务的优先级。configMAX_PRIORITIES=7,合法 0..6。我们要的是一个明显的梯度:
 * High 最高、Mid 居中、Low 最低,反转才演得真切——Mid 必须压得过 Low、又压不过 High。 */
#define prioHIGH    ( tskIDLE_PRIORITY + 5 )
#define prioMID     ( tskIDLE_PRIORITY + 3 )
#define prioLOW     ( tskIDLE_PRIORITY + 1 )

/* 起跑时刻(相对启动的 ms)。三段错开,保证「Low 先持锁 → High 撞锁 → Mid 插队」因果。
 * Low 启动后立刻拿锁,然后拖到 LOW_HOLD_MS 才放;High 在 LOW 拿锁之后才起跑、必然撞锁;
 * Mid 在 High 撞锁之后才起跑、必然能插队。整个剧情刻意压在 ~3.5s 内收尾,这样
 * `stdbuf -oL timeout 4` 一把就能抓全 High 拿到锁那一刻——对比才看得完整。 */
#define LOW_START_MS        ( 300 )
#define HIGH_START_MS       ( 600 )
#define MID_START_MS        ( 900 )

/* Low 持锁的总时长:从它拿锁算起,拖到这时才 give。这个窗口必须覆盖「High 撞锁 + Mid 插队」
 * 全程,否则 Low 太早放锁,反转来不及演完。设成 1200ms,留足余地。 */
#define LOW_HOLD_MS         ( 1200 )

/* Mid 纯忙等的总时长。故意做成既不让出 CPU、也不碰锁的长任务,长到足以在二值信号量下
 * 把 High 间接饿死一截(必须 > LOW_HOLD_MS:这样二值那段里 Mid 跑完前 Low 根本回不来、
 * High 自然也拿不到锁)。2500ms 够长,且让二值剧情在 ~3.4s 处收尾、落在 timeout 4 内。 */
#define MID_BURN_MS         ( 2500 )

/* 共享资源锁。类型是抽象的 SemaphoreHandle_t——二值信号量和互斥量都用它,所以下面
 * 只在创建处分流,业务代码里 take/give 一字不改。这就是「二值信号量 vs 互斥量」在
 * API 层面唯一的差别:创建函数不同;真正决定性的差别(优先级继承)藏在内核里。 */
static SemaphoreHandle_t xResourceLock = NULL;

/* 把 tick 翻成 ms 打印,时间戳更直观。xTaskGetTickCount() 在 configTICK_RATE_HZ=1000 下
 * 正好 1 tick=1ms,直接换算即可。 */
static TickType_t xNowMs( void )
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* 共享资源临界区里干的那点「活」。故意拆成「打印进 + 打印出」两行,夹住一段
 * vTaskDelay,这样输出里能清楚看到「谁在持锁、持了多久」。注意这里的 vTaskDelay
 * 本身就会让出 CPU——它不是问题所在;问题在于「Low 拿不到 CPU 去执行到这一行 give」,
 * 而那是 Mid 抢 CPU 造成的。 */
static void prvUseSharedResource( const char *pcWho )
{
    console_print( "[%6lu ms] %s: INSIDE critical section, holding lock\n",
                   ( unsigned long ) xNowMs(), pcWho );
    vTaskDelay( pdMS_TO_TICKS( 60 ) );
    console_print( "[%6lu ms] %s: leaving critical section\n",
                   ( unsigned long ) xNowMs(), pcWho );
}

/* Low 任务:启动后先睡到 LOW_START_MS,醒来拿锁、持锁 LOW_HOLD_MS(期间故意磨蹭),
 * 然后放锁。它是反转剧本里「先持锁的那个倒霉蛋」。 */
static void prvLowTask( void *pvParameters )
{
    ( void ) pvParameters;

    vTaskDelay( pdMS_TO_TICKS( LOW_START_MS ) );

    console_print( "[%6lu ms] Low: trying to TAKE lock\n", ( unsigned long ) xNowMs() );
    xSemaphoreTake( xResourceLock, portMAX_DELAY );
    console_print( "[%6lu ms] Low: lock TAKEN\n", ( unsigned long ) xNowMs() );

    prvUseSharedResource( "Low" );

    /* 持锁期间故意再拖一会儿,留出窗口让 High 撞锁、Mid 插队,反转才演得出来。
     * 注意:在二值信号量下,这一段 vTaskDelay 期间 Low 被 Mid 抢走 CPU、醒不来,
     * 它根本走不到下面那行 give——这正是 High 被间接饿死的根。 */
    vTaskDelay( pdMS_TO_TICKS( LOW_HOLD_MS ) );

    console_print( "[%6lu ms] Low: GIVING lock\n", ( unsigned long ) xNowMs() );
    xSemaphoreGive( xResourceLock );

    console_print( "[%6lu ms] Low: done, deleting self\n", ( unsigned long ) xNowMs() );
    vTaskDelete( NULL );
}

/* High 任务:睡到 HIGH_START_MS(此时 Low 早已持锁),醒来拿锁、必然撞锁 blocked。
 * 它拿不到锁的那一刻就是反转的开端。拿到锁后用它一小会儿就放,然后自我销毁。 */
static void prvHighTask( void *pvParameters )
{
    ( void ) pvParameters;

    vTaskDelay( pdMS_TO_TICKS( HIGH_START_MS ) );

    console_print( "[%6lu ms] High: trying to TAKE lock (blocked expected)\n",
                   ( unsigned long ) xNowMs() );
    TickType_t xWaitStart = xNowMs();

    /* 撞锁:锁在 Low 手里 → blocked。何时醒来,取决于 Low 何时 give,而那又取决于
     * Mid 有没有把 Low 按住——这就是本章要对比的变量。 */
    xSemaphoreTake( xResourceLock, portMAX_DELAY );

    TickType_t xWaited = xNowMs() - xWaitStart;
    console_print( "[%6lu ms] High: lock TAKEN after %lu ms wait\n",
                   ( unsigned long ) xNowMs(), ( unsigned long ) xWaited );

    prvUseSharedResource( "High" );

    xSemaphoreGive( xResourceLock );
    console_print( "[%6lu ms] High: done, deleting self\n", ( unsigned long ) xNowMs() );
    vTaskDelete( NULL );
}

/* Mid 任务:搅局者。睡到 MID_START_MS(此时 Low 持锁、High 已撞锁 blocked),醒来后
 * 纯忙等 MID_BURN_MS,既不让出 CPU、也不碰那把锁。它优先级高于 Low、低于 High,
 * 但此刻 High 在 blocked(等锁),调度器眼里「ready 里最高优先级」就是它——于是它
 * 独占 CPU,把 Low 按在原地动不了,Low 放不了锁,High 也就解脱不了。 */
static void prvMidTask( void *pvParameters )
{
    ( void ) pvParameters;

    vTaskDelay( pdMS_TO_TICKS( MID_START_MS ) );

    console_print( "[%6lu ms] Mid: START long unrelated work (burning %d ms, NOT touching lock)\n",
                   ( unsigned long ) xNowMs(), MID_BURN_MS );

    /* 纯忙等:不开锁、不碰锁、不让出。volatile 防编译器优化掉。 */
    volatile unsigned long ulJunk = 0;
    TickType_t xStart = xTaskGetTickCount();
    while( ( xTaskGetTickCount() - xStart ) < pdMS_TO_TICKS( MID_BURN_MS ) )
    {
        ulJunk += 1;
    }
    ( void ) ulJunk;

    console_print( "[%6lu ms] Mid: long work DONE\n", ( unsigned long ) xNowMs() );
    vTaskDelete( NULL );
}

int main( void )
{
    /* 关掉 stdout 全缓冲,避免重定向/管道时输出丢失(见 02_environment 排坑章)。 */
    setvbuf( stdout, NULL, _IOLBF, 0 );

    console_init();

    /* 唯一的分流点:用哪种锁。
     *   - 不定义 USE_MUTEX → xSemaphoreCreateBinary():刚创建是「空」的,必须先 give 一下
     *     让它变「满」,任务才能 take 到(二值信号量的经典初值坑)。它没有优先级继承。
     *   - 定义 USE_MUTEX → xSemaphoreCreateMutex():创建即「满」,可直接 take;
     *     且内核会做优先级继承。 */
    #ifdef USE_MUTEX
        xResourceLock = xSemaphoreCreateMutex();
        console_print( "08_resources: lock = MUTEX (priority inheritance ON)\n" );
    #else
        xResourceLock = xSemaphoreCreateBinary();
        /* 二值信号量创建后是「空」,先 give 一次让它变「满」,后续 Low 才能 take 到。 */
        xSemaphoreGive( xResourceLock );
        console_print( "08_resources: lock = BINARY SEMAPHORE (NO priority inheritance)\n" );
    #endif
    configASSERT( xResourceLock != NULL );

    xTaskCreate( prvLowTask,  "Low",  configMINIMAL_STACK_SIZE, NULL, prioLOW,  NULL );
    xTaskCreate( prvMidTask,  "Mid",  configMINIMAL_STACK_SIZE, NULL, prioMID,  NULL );
    xTaskCreate( prvHighTask, "High", configMINIMAL_STACK_SIZE, NULL, prioHIGH, NULL );

    console_print( "08_resources: starting scheduler (prio High=%d Mid=%d Low=%d)\n",
                   prioHIGH, prioMID, prioLOW );

    vTaskStartScheduler();

    for( ; ; )
    {
    }

    return 0;
}
