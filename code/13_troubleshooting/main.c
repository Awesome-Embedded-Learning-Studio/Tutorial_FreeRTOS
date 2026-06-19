/*
 * 13_troubleshooting —— 排错章独立 demo。
 *
 * 排错是横向能力:它不属于某一条原语,而是把前面所有章(任务/队列/信号量/互斥量/内存/
 * 调试)里你迟早会踩的坑,系统化地「复现 + 诊断 + 对策」一遍。这个 demo 的组织方式和前面
 * 的 demo 不一样——它不是一个常驻多任务应用,而是一个「故障博物馆」:由一个 DriverTask
 * 顺序跑几个互相隔离的小场景,每个场景复现一类经典故障,在输出里打印「现象 → 原因 →
 * 对策」三段式,跑完一个场景就把它建出来的任务/原语清掉,再进下一个。这样所有故障能在
 * 一次 build/run 里走完,而且彼此不串味(不会因为 A 场景挂了拖死 B 场景)。
 *
 * 五个场景,对应文档那张速查表的几大类:
 *   场景 1 —— 内存余量观测 / malloc failed 怎么「看见」。
 *     heap_4 + 2MB 下我们不会真把堆撑爆(撑爆会进 vApplicationMallocFailedHook →
 *     vAssertCalled 死循环,demo 就停了,没法继续演后面的)。所以这一幕演的是
 *     「怎么用 xPortGetMinimumEverFreeHeapSize 提前看见堆在往哪走」——这是 malloc failed
 *     的「预防性对策」,比等 hook 炸了再排强得多。真正的 hook 触发现场我们只在输出里描述。
 *   场景 2 —— 栈溢出在 host 下检测失效,怎么手动发现。
 *     一个任务故意深递归吃栈,演示 configCHECK_FOR_STACK_OVERFLOW 在 POSIX port 不触发、
 *     但 uxTaskGetStackHighWaterMark 能量到水位在掉。对策就是「别指望 hook,靠水位探针 +
 *     真硬件上把 configCHECK_FOR_STACK_OVERFLOW 开起来」。
 *   场景 3 —— 优先级反转 + starvation。
 *     低优任务持二值信号量、中优任务忙等抢占、高优任务撞在信号量上,高优被中优拖死——
 *     这是经典反转。同一个场景同时演 starvation(中优忙等不让出,把低优饿死,信号量永远
 *     还不掉)。对策:把二值信号量换成 mutex(带优先级继承),或在忙等里 taskYIELD/vTaskDelay。
 *   场景 4 —— 队列满丢消息。
 *     快生产者往容量很小的队列狂 xQueueSend(零超时),慢消费者来不及取 → 满了之后 send
 *     返回 errQUEUE_FULL,消息被丢。对策:用 xQueueOverwrite(最新值语义,丢最旧)、
 *     或 xQueueSend 带阻塞、或用计数信号量做配额限速。
 *   场景 5 —— 死锁(锁顺序相反)。
 *     两个任务各拿一把 mutex 后去抢对方的另一把,顺序相反 → 互相等 → 死锁。我们用带超时
 *     的 take 来「探测」死锁(到点拿不到说明卡住了),而不是真死在那儿把 demo 拖死。对策:
 *     全局统一的锁获取顺序,或用超时 + 回退。
 *
 * 设计上几个关键取舍,值得交代:
 *   - 所有「会挂掉」的故障(真 malloc failed、真死锁死等)都「不真挂」,改成用超时探测 +
 *     打印诊断。因为这是一个顺序跑完的故障博物馆,任何一个场景真挂了,后面的就演示不了;
 *     而且真挂的表现(demo 卡住、Ctrl-c 才能退)对读者也不友好。用超时探测既复现了「卡住
 *     的现象」、又能继续往下走,这才是教学 demo 该有的样子。真硬件调试时你照样会看到真挂,
 *     但那时你已经有这张「现象清单」对照了。
 *   - 场景之间靠 vTaskDelay 隔开一段时间 + 用一个 g_ulStage 全局阶段号互斥(每个场景的
 *     子任务只在属于自己的阶段号里跑),保证前一个场景的子任务彻底退干净(它 vTaskDelete
 *     自己)再进下一个。
 *
 * 所有 FreeRTOS 必需的回调样板(vAssertCalled / 各类 hook / 静态分配内存)都在 app_hooks.c
 * 里,这个文件只放 demo 本身的逻辑。CMakeLists 已选 heap_4 + 2MB(见 spec)。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "console.h"

/* DriverTask 协调五个场景的节奏:每场景给它这么多 ms 跑完再进下一个。 */
#define STAGE_DURATION_MS    ( 1200 )

/* 场景之间留一点空档,让前一个场景的子任务 vTaskDelete 自身 + idle 回收干净。 */
#define INTER_STAGE_GAP_MS   ( 300 )

/* DriverTask 优先级:压在所有「被演示」的高优任务之上,这样它能准时推进场景切换、
 * 不会被某个忙等的高优子任务拖住。configMAX_PRIORITIES=7,合法 0..6,给 +5。 */
#define prioDRIVER    ( tskIDLE_PRIORITY + 5 )

/* 各场景子任务用的优先级档位(故意拉开差距好演反转/starvation)。 */
#define prioHIGH      ( tskIDLE_PRIORITY + 4 )
#define prioMID       ( tskIDLE_PRIORITY + 3 )
#define prioLOW       ( tskIDLE_PRIORITY + 2 )

/* 全局阶段号:DriverTask 进入一个场景时把它写成本场景的编号,该场景的子任务据此
 * 判断「现在轮到我了吗」;切换场景时 DriverTask 把它改掉,上一场景残留的子任务
 * 看到号不对就知道自己该退了。这避免了多场景子任务串味。 */
static volatile uint32_t g_ulStage = 0;

/* 场景编号常量,既是 g_ulStage 的值,也用来打日志标签。 */
#define STAGE_HEAP        ( 1 )
#define STAGE_STACK       ( 2 )
#define STAGE_INVERSION   ( 3 )
#define STAGE_QUEUE_FULL  ( 4 )
#define STAGE_DEADLOCK    ( 5 )

/* ──────────────────────────────────────────────────────────────────────────
 * 场景 1:内存余量观测 / malloc failed 怎么「看见」
 *
 * 我们不在 demo 里真把 heap_4 那 2MB 撑爆——撑爆会进 vApplicationMallocFailedHook,
 * 而 app_hooks.c 里那个 hook 调 vAssertCalled 进死循环,demo 就此停住、再也演不了后面。
 * 这恰恰是「malloc failed 的现象」之一:hook 触发 → 程序停在 assert。所以真正的 hook 现场
 * 我们只在打印里描述,这里演的是更值钱的东西——**预防**:用 xPortGetMinimumEverFreeHeapSize
 * 在 malloc failed 还没发生时就读出「堆已经吃到了哪里」,这是把 malloc failed 消灭在发生前
 * 的核心观测手段(heap_3 不提供这俩函数,所以我们前面才坚持用 heap_4)。
 * ------------------------------------------------------------------------- */
static void prvStageHeap( void )
{
    console_print( "\n[stage 1] heap instrumentation —— how to SEE malloc-failure coming\n" );

    /* 记下进场时的水位,作为对照基线。free 是「此刻还剩多少」,
     * min_ever 是「自调度器启动以来,堆曾经掉到的最低点」——后者才是排 malloc failed
     * 时最该盯的数:它告诉你「最紧张的那一下离撑爆还有多远」。 */
    size_t ulFreeBefore = xPortGetFreeHeapSize();
    size_t ulMinBefore = xPortGetMinimumEverFreeHeapSize();
    console_print( "  free=%lu bytes, min_ever=%lu bytes (baseline)\n",
                   ( unsigned long ) ulFreeBefore, ( unsigned long ) ulMinBefore );

    /* 分配几块中等大小的对象,模拟「应用运行中陆续申请内存」。每申请一块,free 往下掉、
     * min_ever 跟着掉(只要 free 创了新低)。我们把这些块指针记下来,一会儿一起还掉,
     * 不制造真碎片——本章不是内存章,不演碎片,只演「怎么读水位」。 */
    #define HEAP_PROBE_BLOCKS    ( 6 )
    #define HEAP_PROBE_BLOCK_SZ   ( 8 * 1024 )    /* 8KB 一块,host 下都是大块,看得清 */
    void *apvBlocks[ HEAP_PROBE_BLOCKS ];
    for( uint32_t i = 0; i < HEAP_PROBE_BLOCKS; i++ )
    {
        apvBlocks[ i ] = pvPortMalloc( HEAP_PROBE_BLOCK_SZ );

        /* 故意演一次「分配结果要检查」:万一某块返回 NULL(堆真不够了),立刻打出来。
         * 正常 2MB 够这 6×8KB=48KB 绰绰有余,这里走不到 NULL 分支;但留下这个检查是
         * 「对策」的范本——应用代码里每次 pvPortMalloc 都该查返回值,而不是默认它成功。 */
        if( apvBlocks[ i ] == NULL )
        {
            console_print( "  block #%lu: pvPortMalloc returned NULL —— malloc failed!\n",
                           ( unsigned long ) i );
        }
    }

    console_print( "  after allocating %u x %u bytes: free=%lu, min_ever=%lu\n",
                   ( unsigned ) HEAP_PROBE_BLOCKS, ( unsigned ) HEAP_PROBE_BLOCK_SZ,
                   ( unsigned long ) xPortGetFreeHeapSize(),
                   ( unsigned long ) xPortGetMinimumEverFreeHeapSize() );

    /* 全部还回去:free 涨回来,但 min_ever 不动(它只记历史最低,不随 free 回升)。这就是
     * 「min_ever 是高水位线」的含义——它反映的是「曾经最紧张的一刻」,即使现在全还了,
     * 那个最低点依然刻在那儿供你判断「堆是不是曾经差一点就爆了」。 */
    for( uint32_t i = 0; i < HEAP_PROBE_BLOCKS; i++ )
    {
        vPortFree( apvBlocks[ i ] );
    }

    console_print( "  after freeing all: free=%lu, min_ever=%lu (min_ever did NOT rise back)\n",
                   ( unsigned long ) xPortGetFreeHeapSize(),
                   ( unsigned long ) xPortGetMinimumEverFreeHeapSize() );
    console_print( "  -> if min_ever ever sits near 0, a malloc-failed is one allocation away.\n"
                   "     (a real malloc failed would call vApplicationMallocFailedHook -> vAssertCalled -> halt)\n" );
}

/* ──────────────────────────────────────────────────────────────────────────
 * 场景 2:栈溢出在 host 下检测失效,怎么手动发现
 *
 * configCHECK_FOR_STACK_OVERFLOW 在真 MCU 上靠「给栈涂魔数、任务切换时检查有没有被踩」
 * 来报警。但 POSIX port 把每个任务映射成一个 pthread,pthread 的栈归宿主 glibc 管,FreeRTOS
 * 内核根本够不着去涂魔数——所以这个 hook 在 host 下永远不触发(见 pitfalls 章)。那 host 下
 * 怎么发现「栈不够用」?答案是 uxTaskGetStackHighWaterMark:它返回「这个任务从启动至今,
 * 栈剩余的最少字数」——水位线。但这里有个 host 模拟的诚实陷阱:POSIX port 把每个任务
 * 映射成一个 pthread,任务真正的调用栈帧压在 **pthread 自己的栈** 上,而 FreeRTOS 给任务
 * 准备的那块 pxStack 缓冲区主要用来存 Thread_t 元数据 + 当大小提示,并不承接实际调用帧。
 * 内核给 pxStack 涂的 0xa5 魔数因此几乎不会被真实的函数调用踩到,uxTaskGetStackHighWaterMark
 * 扫描它,得到的数基本不动——也就是说 **host 下水位线探针也不可信**。所以这一幕我们老实演
 * 出「探针不可信」:深递归使劲吃栈,水位线纹丝不动;再诚实指出 host 下栈溢出基本是「盲区」,
 * 真正可靠的诊断要靠真硬件把 configCHECK_FOR_STACK_OVERFLOW 开起来。这正是排错章该有的
 * 「诚实交代边界」——不演一个漂亮但假的结果糊弄读者。
 * ------------------------------------------------------------------------- */

/* 爆栈探针子任务的句柄(主流程要用它调 uxTaskGetStackHighWaterMark 量水位)。 */
static TaskHandle_t xStackProbeHandle = NULL;

/* 故意深递归:每层压一些栈(局部数组),递归到一定深度才返回。host 下任务栈虚胖到
 * 128KB(configMINIMAL_STACK_SIZE=16384 字 ×8B),所以递归深度给到能让水位线显著下降、
 * 又不至于真爆栈把 pthread 弄崩的程度。volatile 防止编译器把 buf 优化掉。 */
static void prvDeepRecursion( uint32_t ulDepth )
{
    volatile uint8_t buf[ 512 ];    /* 每层吃 512B 栈 */
    buf[ 0 ] = ( uint8_t ) ulDepth;
    buf[ sizeof( buf ) - 1 ] = ( uint8_t ) ( ulDepth + 1 );
    ( void ) buf;

    if( ulDepth > 0 )
    {
        prvDeepRecursion( ulDepth - 1 );
    }
}

/* 爆栈探针任务:只在 STAGE_STACK 阶段跑,做若干轮「深递归吃栈 → 量水位」。预期(host 下):
 * 水位线**不随递归深度变化**——这正是「POSIX port 下水位线探针不可信」的活体证据。
 * 在真硬件上,同样的代码水位线会随深度明显下降,这里不动纯粹是 port 的实现差异。 */
static void prvStackProbeTask( void *pvParameters )
{
    ( void ) pvParameters;
    const uint32_t ulRecursionDepths[] = { 5, 20, 80 };
    const uint32_t ulNumDepths = sizeof( ulRecursionDepths ) / sizeof( ulRecursionDepths[ 0 ] );

    for( uint32_t i = 0; i < ulNumDepths; i++ )
    {
        if( g_ulStage != STAGE_STACK )
        {
            break;    /* 阶段切走了,别再跑了 */
        }

        configSTACK_DEPTH_TYPE ulBefore = uxTaskGetStackHighWaterMark( NULL );
        prvDeepRecursion( ulRecursionDepths[ i ] );
        configSTACK_DEPTH_TYPE ulAfter = uxTaskGetStackHighWaterMark( NULL );

        console_print( "  recursion depth=%3lu: high_water %lu -> %lu words %s\n",
                       ( unsigned long ) ulRecursionDepths[ i ],
                       ( unsigned long ) ulBefore, ( unsigned long ) ulAfter,
                       ( ulAfter == ulBefore ) ? "(UNCHANGED — probe unreliable on POSIX)"
                                               : "(dropped)" );
    }

    vTaskDelete( NULL );
}

static void prvStageStack( void )
{
    console_print( "\n[stage 2] stack overflow —— on POSIX neither the hook NOR the probe fires\n" );
    console_print( "  configCHECK_FOR_STACK_OVERFLOW=%d (hook won't fire on POSIX port regardless)\n",
                   ( int ) configCHECK_FOR_STACK_OVERFLOW );

    xTaskCreate( prvStackProbeTask, "StackProbe", configMINIMAL_STACK_SIZE,
                 NULL, prioMID, &xStackProbeHandle );

    vTaskDelay( pdMS_TO_TICKS( STAGE_DURATION_MS ) );

    /* 阶段结束。StackProbe 任务要么自己 vTaskDelete 了,要么被阶段号变更打断后自删。 */
    xStackProbeHandle = NULL;

    console_print( "  -> host is blind to stack overflow: the hook never fires (POSIX port)\n"
                   "     AND the high-water probe barely moves (real frames live on the pthread stack).\n"
                   "     on real HW: set configCHECK_FOR_STACK_OVERFLOW=2 — both the hook AND the\n"
                   "     probe work there, because the kernel owns the task stack directly.\n" );
}

/* ──────────────────────────────────────────────────────────────────────────
 * 场景 3:优先级反转 + starvation
 *
 * 经典三任务反转:Low 拿了信号量去「干长活」,High 随后要这把信号量被阻塞,这时 Mid
 * 来了——Mid 优先级高于 Low、低于 High,它一就绪就把 Low 抢走了,Low 没法继续干、
 * 还不掉信号量,High 就被 Mid 活活拖住。这就是「优先级反转」:本来 Mid 不该影响 High,
 * 却因为 Low 夹在中间持锁,把 High 拖给了 Mid。
 *
 * 我们用「二值信号量」(不带优先级继承)来复现,所以反转会发生。对策就是把二值信号量
 * 换成 mutex:xSemaphoreCreateMutex 建出来的 mutex 带优先级继承——Low 持锁时被 High
 * 撞上,Low 会被临时抬到 High 的优先级,Mid 就抢不动它了,Low 赶紧干完还锁,High 早拿锁。
 * 本场景 demo 里我们只复现「反转」现象(用超时探测 High 被拖了多久),对策(换 mutex)
 * 在打印里讲清楚;真要演示继承的修复,详见仓库 08_resources 的独立 demo。
 *
 * 同时这个场景顺手演 starvation:Mid 用忙等不让出,不光拖 High,也把 Low 饿死(Low 还
 * 没机会还锁)——「忙等的高优任务饿死一切低于它的任务」就是 starvation 的标准长相。
 * ------------------------------------------------------------------------- */

/* 反转用的共享资源(二值信号量,故意不带优先级继承,所以反转会发生)。 */
static SemaphoreHandle_t xSharedBinSem = NULL;
/* 记录 High 任务从「想要锁」到「拿到锁」等了多少 tick,供主流程量反转的延迟代价。 */
static volatile TickType_t g_xHighWaitTicks = 0;

/* Low 任务:拿到信号量后「干长活」(忙等一段时间模拟持有锁干慢活),然后还掉。
 * 干活期间它持有信号量不放——这正是反转发生的温床。 */
static void prvLowHolderTask( void *pvParameters )
{
    ( void ) pvParameters;
    /* Low 先 yield 一下,让 High 先就位、先在 take 上阻塞,这样反转的时间线才典型。 */
    vTaskDelay( pdMS_TO_TICKS( 50 ) );

    console_print( "  [LOW] acquiring binary semaphore, will hold it while working...\n" );
    xSemaphoreTake( xSharedBinSem, portMAX_DELAY );

    /* 忙等一段模拟「持锁干慢活」。host 下 tick=1ms,忙等 300ms。 */
    TickType_t xStart = xTaskGetTickCount();
    while( ( xTaskGetTickCount() - xStart ) < pdMS_TO_TICKS( 300 ) )
    {
        /* 故意不让出:这就是「持锁期间长忙等」,反转的根因之一。 */
    }

    console_print( "  [LOW] work done, releasing semaphore\n" );
    xSemaphoreGive( xSharedBinSem );
    vTaskDelete( NULL );
}

/* High 任务:启动后稍等(Low 先拿锁),然后去 take 同一把信号量——会被阻塞,等 Low 还。
 * 量自己从 take 到拿到等了多久,这就是被 Mid 反转拖出来的延迟。 */
static void prvHighWaiterTask( void *pvParameters )
{
    ( void ) pvParameters;
    vTaskDelay( pdMS_TO_TICKS( 100 ) );    /* 等 Low 先拿到锁 */

    console_print( "  [HIGH] need the resource, blocking on semaphore...\n" );
    TickType_t xBefore = xTaskGetTickCount();
    xSemaphoreTake( xSharedBinSem, portMAX_DELAY );
    g_xHighWaitTicks = xTaskGetTickCount() - xBefore;

    console_print( "  [HIGH] got it after %lu ms (if >>0, priority inversion happened)\n",
                   ( unsigned long ) g_xHighWaitTicks );
    xSemaphoreGive( xSharedBinSem );
    vTaskDelete( NULL );
}

/* Mid 任务:在 Low 持锁干活的窗口里「上线」忙等,抢占 Low,造成反转。 */
static void prvMidMeddlerTask( void *pvParameters )
{
    ( void ) pvParameters;
    vTaskDelay( pdMS_TO_TICKS( 150 ) );    /* 等 Low 拿锁、High 阻塞后,自己再上线 */

    console_print( "  [MID] preempting and busy-spinning (starves LOW, blocks HIGH indirectly)\n" );
    /* 忙等 250ms:这段里 Mid 一直占着 CPU(它优先级高于 Low),Low 还不了锁,High 干等。 */
    TickType_t xStart = xTaskGetTickCount();
    while( ( xTaskGetTickCount() - xStart ) < pdMS_TO_TICKS( 250 ) )
    {
    }
    vTaskDelete( NULL );
}

static void prvStageInversion( void )
{
    console_print( "\n[stage 3] priority inversion + starvation (binary semaphore, no inheritance)\n" );
    g_xHighWaitTicks = 0;

    xSharedBinSem = xSemaphoreCreateBinary();
    configASSERT( xSharedBinSem != NULL );
    xSemaphoreGive( xSharedBinSem );    /* 二值信号量创建为空,先 give 让它「可用」 */

    xTaskCreate( prvLowHolderTask, "Low", configMINIMAL_STACK_SIZE, NULL, prioLOW, NULL );
    xTaskCreate( prvMidMeddlerTask, "Mid", configMINIMAL_STACK_SIZE, NULL, prioMID, NULL );
    xTaskCreate( prvHighWaiterTask, "High", configMINIMAL_STACK_SIZE, NULL, prioHIGH, NULL );

    vTaskDelay( pdMS_TO_TICKS( STAGE_DURATION_MS ) );

    console_print( "  -> fix: use xSemaphoreCreateMutex() instead — it does priority inheritance,\n"
                   "     so LOW is boosted to HIGH's priority and Mid can't preempt it.\n" );

    vSemaphoreDelete( xSharedBinSem );
    xSharedBinSem = NULL;
}

/* ──────────────────────────────────────────────────────────────────────────
 * 场景 4:队列满丢消息
 *
 * 快生产者往容量很小的队列狂 xQueueSend(零超时,即「队列满就立刻返回失败、不等」),
 * 慢消费者来不及取,队列很快满,之后生产者每次 send 都返回 errQUEUE_FULL ——消息被丢。
 * 这是最常见的「队列丢消息」原因,不是 bug,是「无阻塞 send 撞上满队列」的语义后果。
 * 我们在输出里数「成功投递了多少、丢了多少」,让丢消息肉眼可见。
 *
 * 三种对策(打印里讲):(1) 最新值语义 xQueueOverwrite(丢最旧,容量必须为1);
 * (2) xQueueSend 带阻塞超时(满了就等,生产者被反压);(3) 用计数信号量做配额限速。
 * ------------------------------------------------------------------------- */

#define FULL_QUEUE_LENGTH    ( 2 )      /* 故意只给 2 格,生产者一狂就满 */
static QueueHandle_t xSmallQueue = NULL;
static volatile uint32_t g_ulSent = 0;    /* 尝试投递的次数 */
static volatile uint32_t g_ulDropped = 0; /* 其中因 errQUEUE_FULL 丢掉的次数 */
/* 生产者/消费者句柄:阶段结束时改阶段号让它们自删,再 vTaskDelay 充足时间确认退干净,
 * 最后才 vQueueDelete——否则消费者还在 xQueueReceive 上睡、队列却被删掉,会撞 configASSERT
 * (xQueueReceive 传 NULL 句柄即断言)。这是个真实的「资源生命周期 vs 任务生命周期」坑。 */
static TaskHandle_t xProducerHandle = NULL;
static TaskHandle_t xConsumerHandle = NULL;

/* 快生产者:死命往队列里 send(零超时),满了就丢。统计 send 成功/失败次数。 */
static void prvFastProducerTask( void *pvParameters )
{
    ( void ) pvParameters;
    uint32_t ulValue = 0;

    while( g_ulStage == STAGE_QUEUE_FULL )
    {
        ulValue++;
        g_ulSent++;
        if( xQueueSend( xSmallQueue, &ulValue, 0 ) != pdPASS )
        {
            g_ulDropped++;    /* 队列满,这一帧丢了 */
        }
        /* 极短的忙等节奏,不 vTaskDelay,模拟「生产远快于消费」。 */
        for( volatile uint32_t i = 0; i < 5000; i++ )
        {
        }
    }
    vTaskDelete( NULL );
}

/* 慢消费者:周期性从队列取,每次取完睡一觉,制造「消费跟不上生产」。 */
static void prvSlowConsumerTask( void *pvParameters )
{
    ( void ) pvParameters;
    uint32_t ulGot;

    while( g_ulStage == STAGE_QUEUE_FULL )
    {
        if( xQueueReceive( xSmallQueue, &ulGot, 0 ) == pdPASS )
        {
            /* 取到了,睡一觉模拟「慢慢处理」。 */
        }
        vTaskDelay( pdMS_TO_TICKS( 80 ) );
    }
    vTaskDelete( NULL );
}

static void prvStageQueueFull( void )
{
    console_print( "\n[stage 4] queue full —— non-blocking send drops messages\n" );
    g_ulSent = 0;
    g_ulDropped = 0;

    xSmallQueue = xQueueCreate( FULL_QUEUE_LENGTH, sizeof( uint32_t ) );
    configASSERT( xSmallQueue != NULL );

    xTaskCreate( prvSlowConsumerTask, "Consumer", configMINIMAL_STACK_SIZE, NULL, prioMID, &xConsumerHandle );
    xTaskCreate( prvFastProducerTask, "Producer", configMINIMAL_STACK_SIZE, NULL, prioLOW, &xProducerHandle );

    vTaskDelay( pdMS_TO_TICKS( STAGE_DURATION_MS ) );

    console_print( "  producer attempted %lu sends, %lu dropped (errQUEUE_FULL)\n",
                   ( unsigned long ) g_ulSent, ( unsigned long ) g_ulDropped );
    console_print( "  -> fixes: (a) xQueueOverwrite for latest-value semantics (capacity 1),\n"
                   "     (b) xQueueSend with a block timeout to back-pressure the producer,\n"
                   "     (c) counting-semaphore token budget to gate the producer.\n" );

    /* 关键的收尾顺序:消费者任务在自己的 80ms vTaskDelay 上睡着、还没机会看到阶段号变更、
     * 还没 vTaskDelete 自己。如果此时就 vQueueDelete,消费者醒来 xQueueReceive 会撞上已删的
     * 队列 → configASSERT(pxQueue)。所以必须先改阶段号(让消费者的 while(阶段==本场景)循环
     * 退出、自删),再等够时间(> 80ms 睡眠)确认它退干净,最后才删队列。这就是「资源生命周期
     * 要覆盖所有用它的任务」的现场——也是本章一个真实的、容易踩的坑。 */
    g_ulStage = 0;    /* 退出本场景:消费者/生产者下次循环判到就 vTaskDelete 自己 */
    vTaskDelay( pdMS_TO_TICKS( 150 ) );    /* > 消费者 80ms 睡眠,确保它跑完一轮自删 */

    vQueueDelete( xSmallQueue );
    xSmallQueue = NULL;
    xConsumerHandle = NULL;
    xProducerHandle = NULL;
}

/* ──────────────────────────────────────────────────────────────────────────
 * 场景 5:死锁(锁顺序相反)
 *
 * 两个任务各先拿一把 mutex、再去抢对方的另一把,且两人拿锁的顺序相反(TaskA 先 A 后 B,
 * TaskB 先 B 后 A),就构成经典的「环形等待」死锁:A 拿着 A 等 B、B 拿着 B 等 A,谁也不撒手。
 * 真死锁会让两个任务永远 blocked,demo 就卡死。所以我们用「带超时的 take」来探测:第二个
 * take 如果到点还拿不到,就说明卡在环形等待里了——这是「用超时诊断死锁」的标准做法。
 * 对策是「全局统一的锁顺序」:所有任务都按同一个顺序(比如永远先 A 后 B)拿锁,环形等待
 * 就构不成。下面 TaskB 故意反着拿,制造死锁;我们在输出里量到「第二次 take 超时」,证据确凿。
 * ------------------------------------------------------------------------- */

static SemaphoreHandle_t xMutexA = NULL;
static SemaphoreHandle_t xMutexB = NULL;
static volatile BaseType_t g_xDeadlockDetected = pdFALSE;

/* TaskA:先拿 A,再去拿 B(顺序 A→B,这是「正确」的顺序)。 */
static void prvLockOrderATask( void *pvParameters )
{
    ( void ) pvParameters;

    vTaskDelay( pdMS_TO_TICKS( 50 ) );    /* 错开一点,让两个任务的拿锁交错 */

    xSemaphoreTake( xMutexA, portMAX_DELAY );
    console_print( "  [A] locked A, now wants B\n" );
    vTaskDelay( pdMS_TO_TICKS( 100 ) );    /* 拿了 A 之后停一下,给 B 先拿 B 的机会 */

    /* 带 300ms 超时去拿 B:正常情况下(无死锁)很快拿到;若 TaskB 已拿 B 且在等 A,
     * 这里就会超时——死锁被探测到。 */
    if( xSemaphoreTake( xMutexB, pdMS_TO_TICKS( 300 ) ) == pdPASS )
    {
        console_print( "  [A] locked B too (no deadlock)\n" );
        xSemaphoreGive( xMutexB );
    }
    else
    {
        console_print( "  [A] timed out waiting for B —— DEADLOCK detected!\n" );
        g_xDeadlockDetected = pdTRUE;
    }
    xSemaphoreGive( xMutexA );
    vTaskDelete( NULL );
}

/* TaskB:先拿 B,再去拿 A(顺序 B→A,故意和 A 相反,构成环形等待)。 */
static void prvLockOrderBTask( void *pvParameters )
{
    ( void ) pvParameters;

    xSemaphoreTake( xMutexB, portMAX_DELAY );
    console_print( "  [B] locked B, now wants A (reverse order -> circular wait)\n" );
    vTaskDelay( pdMS_TO_TICKS( 100 ) );

    if( xSemaphoreTake( xMutexA, pdMS_TO_TICKS( 300 ) ) == pdPASS )
    {
        console_print( "  [B] locked A too (no deadlock)\n" );
        xSemaphoreGive( xMutexA );
    }
    else
    {
        console_print( "  [B] timed out waiting for A —— DEADLOCK detected!\n" );
        g_xDeadlockDetected = pdTRUE;
    }
    xSemaphoreGive( xMutexB );
    vTaskDelete( NULL );
}

static void prvStageDeadlock( void )
{
    console_print( "\n[stage 5] deadlock —— reverse lock ordering (detected via timeout)\n" );
    g_xDeadlockDetected = pdFALSE;

    xMutexA = xSemaphoreCreateMutex();
    xMutexB = xSemaphoreCreateMutex();
    configASSERT( xMutexA != NULL && xMutexB != NULL );

    xTaskCreate( prvLockOrderATask, "LockA", configMINIMAL_STACK_SIZE, NULL, prioMID, NULL );
    xTaskCreate( prvLockOrderBTask, "LockB", configMINIMAL_STACK_SIZE, NULL, prioMID, NULL );

    vTaskDelay( pdMS_TO_TICKS( STAGE_DURATION_MS ) );

    console_print( "  deadlock %s\n",
                   g_xDeadlockDetected ? "DETECTED (timed-out second take proved the circular wait)"
                                       : "NOT detected (unexpected)" );
    console_print( "  -> fix: enforce a single global lock-acquisition order (always A before B)\n"
                   "     so a circular wait can never form. Timeout-based takes catch it in debug.\n" );

    vSemaphoreDelete( xMutexA );
    vSemaphoreDelete( xMutexB );
    xMutexA = NULL;
    xMutexB = NULL;
}

/* ──────────────────────────────────────────────────────────────────────────
 * DriverTask:顺序跑完五个场景。每个场景:置阶段号 → 调用该场景的 prvStageXxx(它会
 * 建子任务、观察、清掉子任务)→ 阶段号变更让残留子任务自删 → 进下一场景。
 * ------------------------------------------------------------------------- */
static void prvDriverTask( void *pvParameters )
{
    ( void ) pvParameters;

    /* 给调度器一点时间稳定下来再开场。 */
    vTaskDelay( pdMS_TO_TICKS( 200 ) );

    console_print( "13_troubleshooting: fault museum, running 5 stages sequentially\n" );

    g_ulStage = STAGE_HEAP;
    prvStageHeap();
    vTaskDelay( pdMS_TO_TICKS( INTER_STAGE_GAP_MS ) );

    g_ulStage = STAGE_STACK;
    prvStageStack();
    vTaskDelay( pdMS_TO_TICKS( INTER_STAGE_GAP_MS ) );

    g_ulStage = STAGE_INVERSION;
    prvStageInversion();
    vTaskDelay( pdMS_TO_TICKS( INTER_STAGE_GAP_MS ) );

    g_ulStage = STAGE_QUEUE_FULL;
    prvStageQueueFull();
    vTaskDelay( pdMS_TO_TICKS( INTER_STAGE_GAP_MS ) );

    g_ulStage = STAGE_DEADLOCK;
    prvStageDeadlock();
    vTaskDelay( pdMS_TO_TICKS( INTER_STAGE_GAP_MS ) );

    console_print( "\n13_troubleshooting: all 5 stages done. end of fault museum.\n" );

    /* 跑完自删,系统回到 idle。 */
    vTaskDelete( NULL );
}

int main( void )
{
    /* 关掉 stdout 全缓冲,避免重定向/管道时输出丢失(见 02_environment 排坑章)。 */
    setvbuf( stdout, NULL, _IOLBF, 0 );

    console_init();

    xTaskCreate( prvDriverTask, "Driver", configMINIMAL_STACK_SIZE,
                 NULL, prioDRIVER, NULL );

    console_print( "13_troubleshooting: starting scheduler (heap=%lu bytes, heap_4)\n",
                   ( unsigned long ) configTOTAL_HEAP_SIZE );

    vTaskStartScheduler();

    for( ; ; )
    {
    }

    return 0;
}
