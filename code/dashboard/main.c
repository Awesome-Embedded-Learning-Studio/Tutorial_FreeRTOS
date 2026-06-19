/*
 * dashboard —— 贯穿全教程的多任务传感器仪表盘的渐进脊柱。
 * 起点是一个心跳任务;[03] 加了内存余量监控任务;
 * [04] 把单一心跳拆成 sensor 采集任务 + display 显示任务(当时用裸全局变量
 * 传递最新读数,仅为立起多任务骨架);[05] 把那根不安全的裸全局变量换成队列,
 * sensor 任务每采到一帧就经队列发给 display 任务,采集→队列→显示;
 * [06] 用一个周期软件定时器驱动 sensor 采样,替代 sensor 任务里原来的
 * vTaskDelay 轮询——定时器到期→回调发通知→sensor 任务被唤醒采样。
 *
 * [07] 本章增量:模拟一个「按钮中断」。sensor 任务的唤醒源在 [06]「周期滴答」
 * 之外,再增加一个「按钮」事件。具体做法是把 [06] 那个发「任务通知」的周期滴答
 * 回调改成 give 一把二值信号量(xSampleSemaphore),另起一把二值信号量
 * xButtonSemaphore 由一个「按钮源」定时器的回调 give——这两把 give 都走
 * xSemaphoreGiveFromISR(类 ISR 上下文里 give 信号量,正是本章教的中断↔任务同步
 * 用法)。sensor 任务在周期滴答信号量上阻塞(带短超时),每一轮顺手以零超时探一次
 * 按钮信号量;周期滴答来了就采「周期帧」,按钮事件来了就额外采一帧并标 [BUTTON!]。
 * 这样「周期采样」和「按钮中断采样」在输出里一眼分得清。
 *
 * ⚠️ POSIX 边界(诚实交代,文档专节展开):host 模拟下没有真中断,「按钮」和「滴答」
 * 都是被软件定时器周期触发的,它们的回调跑在定时器服务任务上下文、各自扮演「ISR」
 * 的角色,从那里调 xSemaphoreGiveFromISR 是相对安全的(port 能正确跟踪这个上下文,
 * 不会像「从外部 pthread 直调 FromISR」那样死锁)。但这里**没有用 Queue Set** 来
 * 同时等待两把信号量——实测在 POSIX 移植下,「定时器服务任务里 FromISR + 队列集
 * 等待」的组合会让定时器卡住(stall),这是这套模拟的一个已知脆弱点。所以我们改用
 * 更朴素的「主信号量带短超时阻塞 + 旁路信号量零超时探一下」的写法,既能区分两个
 * 来源、又稳如老狗。这套模式搬到真硬件上,真 ISR 里 give 信号量、处理任务 take,
 * 一行不用改;队列集在真硬件上也没有这个 stall 问题,真硬件上完全可以直接用队列集。
 * 为什么 host 下不能从外部 pthread 直调 FromISR、以及这个队列集 stall,见中断管理章
 * 和仿真坑点章。
 *
 * [07] 的设计要点:
 *   - 新增 xButtonSemaphore + 周期 2500ms 的「按钮源」定时器 xButtonTimer,回调
 *     prvButtonCallback 跑在定时器服务任务上下文,只 xSemaphoreGiveFromISR 一行——
 *     这就是「按钮 ISR」的标准写法(give 信号量即返回)。
 *   - [06] 的周期滴答从「任务通知」改成 xSampleSemaphore(xSemaphoreGiveFromISR),
 *     因为本章要统一用信号量把中断事件从类 ISR 上下文递交到任务,叙事一致。
 *   - sensor 任务:sensor 唤醒源有两把信号量。它主阻塞在周期滴答信号量上、带
 *     SAMPLE_POLL_TIMEOUT_MS 短超时;每一轮结束前用零超时探一下按钮信号量,有就
 *     额外采一帧标 [BUTTON!]。这样按钮事件最长延迟不超过一个轮询超时,既和周期采样
 *     区分,又不依赖(实测不稳的)队列集。
 *
 * [08] 本章增量:给「会被多个任务并发写的共享统计计数」上一把互斥量。
 * sensor 和 display 是两个不同优先级、各自独立调度的任务,它们都往同一组计数器里
 * 写(sensor 每采一帧就把「总采样数」、若是按钮帧还把「按钮采样数」+1;display 每显示
 * 一帧就把「总显示数」+1)。这一组计数器就是典型的共享资源——对 uint32_t 的 ++ 在 C
 * 层面不是原子的(读-改-写三步,可能被抢占在中间),两个任务同时 ++ 就会丢更新。我们用
 * 一把互斥量 xStatsMutex 把「读改写」整段包成临界区:take → 改 → give。这正是资源管理章
 * 教的标准用法。另起一个优先级最低的 Stats 任务,周期性地 take 同一把互斥量、读一份计数
 * 快照、give、再打印——这是「多读一写场景里读者也要持锁」的标准写法,保证它读到的是
 * 一组互相一致的快照(而不是「采样数读到自增前、显示数读到自增后」那种撕裂态)。
 * 这里用互斥量而不用二值信号量,正是因为它带优先级继承:Stats 任务拿锁时若被 sensor
 * 抢、sensor 又撞在这把锁上,sensor 会把 Stats 临时抬到自己的高优先级,让它赶紧 give、
 * 自己早拿锁——资源管理章的优先级继承,在这里是真在起作用的。
 *
 * [09] 本章增量:加一个事件组(Event Group),让 display 任务「等组合条件再刷新」。
 * 起因是个很真实的需求:display 不该在系统还没初始化好(比如显示驱动没就绪)时就
 * 贸然刷屏。所以我们给 display 加一道「初始化完成 AND 有新采样」的组合门——只有
 * 两个条件都成立,它才往下走显示这一帧。这正好演事件组的核心卖点:xEventGroupWaitBits
 * 的 AND 等待(等一组位「全部」置位才解除阻塞)。我们用一块事件组 xBootEvents,两位:
 *   BIT_INIT_DONE (bit0) —— 「初始化完成」。一个 InitTask 延时 INIT_DONE_DELAY_MS 后
 *     set 它(模拟「系统初始化要花点时间」)。它是一块「常驻门」:一旦 set 就不清,
 *     display 每次等到的 AND 条件里它都得在。
 *   BIT_NEW_SAMPLE (bit1) —— 「有新采样」。sensor 每往队列投一帧,就顺手 set 它一下,
 *     告诉 display「队列里有新货」。display 消费一帧后手动 xEventGroupClearBits 清掉它。
 * display 的循环就一行组合等待:xEventGroupWaitBits( INIT | NEW_SAMPLE, clearOnExit=pdFALSE,
 * waitAll=pdTRUE ),两个位都到位才返回;返回后手动清 NEW_SAMPLE(不清 INIT,让它常驻),
 * 再从队列取帧显示。clearOnExit 故意给 pdFALSE 是为了「只清新采样、不动常驻门」——
 * 这正是事件组「按位清除」的精细控制:xEventGroupWaitBits 的 xClearOnExit 是「一刀切清
 * 掉所有等到的位」,想精细控制就得手动 ClearBits。
 *
 * 故意把 INIT_DONE_DELAY_MS 设成 1200ms(大于采样周期 800ms),让输出里能亲眼看到:
 * sensor 第一帧(800ms)早就进了队列、也 set 了 BIT_NEW_SAMPLE,但 display 因为
 * BIT_INIT_DONE 还没到位,AND 等待一直阻塞——直到 1200ms 初始化完成,display 才被
 * 「解锁」、开始显示积压的帧。这就是「AND 组合门」的活体演示。
 *
 * [10] 本章增量:加一个「控制源」任务,用任务通知(xTaskNotify)携带一个动作码,
 * 通知 sensor 任务切换采样模式(normal / boost)。这一步是故意要和前面已有的两种
 * 「轻量唤醒」手段区分开、把任务通知的独有价值演出来:
 *   - [06] 的周期滴答最早用 xTaskNotifyGive(sensor)——但那是「无值的」通知,只是
 *     给传感器拍一下「到点了」,带不了任何数据;[07] 出于叙事一致把它换成了信号量。
 *   - [07] 的按钮事件用二值信号量(xSemaphoreGiveFromISR)——同样是「无值」的,
 *     只能表达「按钮按下了」这一件事,塞不进「按的是哪个按钮 / 要切到哪种模式」。
 *   - 本章用 xTaskNotify 的 eSetValueWithOverwrite 动作,把一个「模式码」
 *     (MODE_NORMAL / MODE_BOOST)写进 sensor 任务的通知值,顺通知一路带过去。这正是
 *     任务通知区别于信号量的关键卖点:**通知值能携带一个 32 位数据**,信号量不能。
 *     不然的话,控制源要表达「切到 boost 模式」就得另开一条带外通道(一个全局变量
 *     + 一个信号量),把「数据」和「通知」拆成两半,既啰嗦又埋下竞态窗口。
 *
 * sensor 任务用 ulTaskNotifyTake(零超时)在每轮循环里探一下自己的通知值:有控制源
 * 下发的新模式码就切模式、清通知值;没有(返回 0)就维持原模式。boost 模式下采到的
 * 那一帧在输出里带 [BOOST] 标记,和普通周期帧、按钮帧三路来源一眼可分。sensor 的
 * 任务通知此前一直空着([06] 的 give 早在 [07] 改成了信号量),所以本章正好把它用起来、
 * 不和那两把信号量抢通道。
 *
 * [12] 本章增量:加一个运行时统计(CPU 占用)周期输出任务 prvCpuStatsTask。
 * dashboard 到这里已经攒了一堆任务(Sensor/Display/HeapMon/Stats/Init/Control + 内核
 * idle/timer daemon),「它们各自吃多少 CPU」是个很自然的可观测性诉求。正好演
 * configGENERATE_RUN_TIME_STATS 的看家本领:开这个开关后,POSIX port 已经替我们接好了
 * 「运行时统计时钟」(portGET_RUN_TIME_COUNTER_VALUE → ulPortGetRunTime,
 * portCONFIGURE_TIMER_FOR_RUN_TIME_STATS 是 no-op),我们只要 uxTaskGetSystemState 拍一张
 * 全任务快照(含 ulRunTimeCounter),用 ulRunTimeCounter*100/总运行时 算出占比打印。
 * 它呼应 [04] 那张「状态快照」(state/prio/freestack),这里补的是「CPU 占用」那一列。
 * 这任务优先级压到最低(同 HeapMon/Stats 一档),只在有空时插进来,否则它自己拍快照、
 * 格式化打印的 CPU 时间会被记进统计、污染数据。
 *
 * [05] 的设计要点(保留):
 *   - 一根采样队列 xSampleQueue 串起 sensor 和 display。sensor 采集到一帧后
 *     xQueueSend 投进去(队列满了就覆盖式丢最旧的一帧,见下),display 在它上面
 *     阻塞 xQueueReceive。
 *   - 用队列而非全局变量的根本原因:队列把「数据拷贝」和「同步」两件事一次做完,
 *     按值拷贝保证「读到的永远是一帧完整的快照」,没有撕裂。
 *   - 投递策略用 xQueueOverwrite(队列容量 1):「最新的采样永远覆盖旧的」,
 *     这是「最新值」语义的经典用法。
 *
 * sensor / display 的优先级沿袭 [04]:sensor 更高(采集时序敏感),
 * display 低一档(显示给人看、不敏感),内存监控最低(只占空闲)。
 *
 * 所有 FreeRTOS 必需的回调样板(vAssertCalled / 各类 hook / 静态分配内存)
 * 都在 app_hooks.c 里,这个文件只放 demo 本身的逻辑。
 *
 * [03] 说明:CMakeLists 已把 FREERTOS_HEAP 选成 4,并把 configTOTAL_HEAP_SIZE
 * 抬到 2MB。原因是 02_environment 那套默认的 heap_3 不实现
 * xPortGetFreeHeapSize / xPortGetMinimumEverFreeHeapSize,而内存监控任务就指着
 * 这俩函数读数;另外 POSIX port 的任务栈一个个都有 16KB+(PTHREAD_STACK_MIN),
 * heap_4 那个固定大小的静态数组若不抬大,连几个任务都装不下。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "timers.h"

#include "console.h"

/* sensor / display 的优先级。configMAX_PRIORITIES=7,合法优先级 0..6。
 * sensor 给 +3,display 给 +2,内存监控保持在最低档 +1(只在有空时跑),
 * [08] 新增的统计任务也压在最低档 +1,确保它只在没有正经任务想跑时才插进来。 */
#define prioSENSOR         ( tskIDLE_PRIORITY + 3 )
#define prioDISPLAY        ( tskIDLE_PRIORITY + 2 )
#define prioHEAP_MONITOR   ( tskIDLE_PRIORITY + 1 )
#define prioSTATS          ( tskIDLE_PRIORITY + 1 )
/* [09] 初始化任务优先级:和 sensor 同档 +3,确保它一到点就及时 set BIT_INIT_DONE、
 * 把 display 的 AND 等待解锁,不被别的活儿拖延。 */
#define prioINIT           ( tskIDLE_PRIORITY + 3 )
/* [10] 控制源任务优先级:压在 sensor(+3)下面一档,只在「下一道模式指令」时短暂
 * 运行,xTaskNotify 一下就走,绝不和 sensor 抢采样时序。 */
#define prioCONTROL        ( tskIDLE_PRIORITY + 2 )

/* [06] 采样定时器周期。沿用 [05] 的 800ms 采样节奏,驱动方式是周期软件定时器。 */
#define SAMPLE_TIMER_PERIOD_MS   ( 800 )

/* [07] 「按钮」源定时器周期。模拟「人隔一会儿按一下按钮」,故意和采样周期错开
 * (2500ms 不是 800ms 的整数倍),这样输出里周期采样和按钮采样交错出现、一眼可分。 */
#define BUTTON_TIMER_PERIOD_MS   ( 2500 )

/* [09] 「初始化完成」的模拟耗时。故意设成 1200ms,大于采样周期 800ms,这样 sensor
 * 的第一帧(800ms)早就进了队列并 set 了 BIT_NEW_SAMPLE,但 display 因为 BIT_INIT_DONE
 * 还没到位,AND 等待一直阻塞——直到 1200ms 初始化完成,display 才被「解锁」。这个
 * 错位让「AND 组合门」的阻塞肉眼可见:队列里有货、但 display 就是不显示。 */
#define INIT_DONE_DELAY_MS    ( 1200 )

/* [10] 控制源下发模式切换的周期。每 1600ms 切一次模式(normal ↔ boost 来回切),
 * 故意和采样周期(800ms)、按钮周期(2500ms)都错开,这样输出里「周期帧 / 按钮帧 /
 * 模式切换」三件事时间上交错、一眼可分。 */
#define CONTROL_PERIOD_MS    ( 1600 )

/* [10] 控制源下发的「采样模式码」。这正是任务通知能携带一个 32 位值的用武之地——
 * 信号量只能 give 一下、带不了码,任务通知把这个码直接写进 sensor 的通知值一路带过去。
 * 用一组非零小整数当码,ulTaskNotifyTake 零超时取到非零值即「有新模式」,取到 0 即
 * 「没有新指令、维持原样」。 */
#define MODE_NORMAL    ( 1 )    /* 普通采样模式:照常采 */
#define MODE_BOOST     ( 2 )    /* 加速采样模式:这一帧带 [BOOST] 标记 */

/* [07] sensor 任务在周期滴答信号量上的短阻塞超时。
 * 周期滴答信号量一般 800ms 来一次,但这一行故意只等 50ms 就醒来——醒来的目的是
 * 顺手去探一下按钮信号量(零超时),保证按钮事件最长 50ms 内被响应,而不是死等
 * 下一个 800ms 周期滴答。这是一个「轻量轮询窗口」:不用队列集,也能及时响应按钮。
 * xSemaphoreTake 超时返回 pdFAIL 是完全正常的,不报错、不停下,只是「这一窗没等到
 * 滴答,那就去探一下按钮再回来等下一窗」。 */
#define SAMPLE_POLL_TIMEOUT_MS   ( 50 )

/* [05] 采样队列容量。这里故意设成 1,配合 xQueueOverwrite 把队列当
 * 「最新值寄存器」用:新采样覆盖旧的,display 永远读到最新一帧、不会积压。
 * 注意:xQueueOverwrite 只允许用在容量为 1 的队列上。 */
#define SAMPLE_QUEUE_LENGTH    ( 1 )

/* [05] sensor 采到、经队列发给 display 的一帧数据。 */
typedef struct
{
    uint32_t ulSeq;       /* 帧序号,单调递增 */
    int32_t  lValue;      /* 模拟传感器读数(这里用一个递增计数器模拟) */
    uint32_t ulTick;      /* 采样时刻的 tick 数,相当时间戳 */
} SampleFrame_t;

/* [05] 采样队列句柄。进 scheduler 之前 xQueueCreate 创建好。 */
static QueueHandle_t xSampleQueue = NULL;

/* [06/07] sensor 的两个唤醒信号,都做成二值信号量。
 *   xSampleSemaphore:[06] 周期采样滴答(原是任务通知,[07] 改信号量以配合本章叙事)。
 *   xButtonSemaphore:[07] 「按钮中断」事件。
 * 两者都由各自的周期定时器回调在「类 ISR 上下文」(定时器服务任务)里 give。 */
static SemaphoreHandle_t xSampleSemaphore = NULL;
static SemaphoreHandle_t xButtonSemaphore = NULL;

/* [09] 事件组:display 用它做「初始化完成 AND 有新采样」的组合等待。
 *   BIT_INIT_DONE (bit0) —— 「初始化完成」。InitTask 延时 INIT_DONE_DELAY_MS 后 set;
 *     这是一块「常驻门」,set 后从不清,display 每次组合等待都要求它在位。
 *   BIT_NEW_SAMPLE (bit1) —— 「有新采样」。sensor 每投一帧就 set 它;display 消费一帧后
 *     手动 ClearBits 清掉,这样「新采样」标记严格随「有没有没消费的帧」走。 */
#define BIT_INIT_DONE      ( 1 << 0 )
#define BIT_NEW_SAMPLE     ( 1 << 1 )
static EventGroupHandle_t xBootEvents = NULL;

/* [08] 共享统计计数器:sensor 任务写 ulTotalSamples(每采一帧 +1)、ulButtonSamples
 * (按钮帧才 +1),display 任务写 ulTotalDisplays(每显示一帧 +1)。多个任务并发写,
 * 所以这一整组计数器用一把互斥量保护。读方(Stats 任务)也要持同一把锁,才能读到
 * 一组「彼此一致」的快照,而不是计数器之间互相不一致的撕裂态。 */
typedef struct
{
    uint32_t ulTotalSamples;   /* sensor 至今采到的帧数(周期+按钮合计) */
    uint32_t ulButtonSamples;  /* 其中按钮触发的帧数 */
    uint32_t ulTotalDisplays;  /* display 至今显示的帧数 */
} StatsCounters_t;
static StatsCounters_t g_xStats = { 0, 0, 0 };

/* [08] 保护 g_xStats 的互斥量。用互斥量(不是二值信号量)就是图它带优先级继承——
 * 持锁者被高优先级任务撞锁时会被临时抬优先级,避免我们在 [08] demo 里演过的那种
 * 优先级反转。 */
static SemaphoreHandle_t xStatsMutex = NULL;

/* [08] 统计任务的打印周期。压在最低优先级,周期拉到 2 秒,只占空闲、不打扰主线。 */
#define STATS_PERIOD_MS    ( 2000 )

/* 定时器句柄。进 scheduler 之前建好。 */
static TimerHandle_t xSampleTimer = NULL;
static TimerHandle_t xButtonTimer = NULL;

/* [10] sensor 任务句柄。控制源任务需要它才能 xTaskNotify 给 sensor——任务通知是
 * 「发给指定任务」的定向原语,必须知道发给谁(不像信号量有个独立对象谁都能 give)。
 * sensor 此前的任务通知值一直空着([06] 的 give 早在 [07] 换成了信号量),本章正好
 * 把它用作「控制源→sensor」的模式切换通道。 */
static TaskHandle_t xSensorTaskHandle = NULL;

/* [10] sensor 当前的采样模式。由控制源任务通过 xTaskNotify 下发的模式码驱动:
 * MODE_NORMAL(普通)或 MODE_BOOST(加速)。boost 模式下采到的帧在输出里带 [BOOST] 标记。
 * 只被 sensor 任务自己读写(单写者),不需要加锁——它是个「当前模式」的局部视图,
 * 控制源只负责把新码写进通知值,真正「切」的动作在 sensor 任务里单线程发生。 */
static uint32_t g_ulSampleMode = MODE_NORMAL;

/* 采一帧并投队列的公共子函数。ulSeq/lValue 由调用方维护并自增,fFromButton 标来源。
 * 抽出来是因为「周期帧」和「按钮帧」采样的活是一样的,只有来源标记不同——
 * 抽成一个函数避免两处重复。
 * [10] 输出里除了来源标记,再带上当前采样模式(普通/boost),这样三路来源 + 模式
 * 标记在输出里一眼可分。 */
static void prvTakeAndEnqueueSample( uint32_t *pulSeq, int32_t *plValue, bool fFromButton )
{
    SampleFrame_t xFrame;
    xFrame.ulSeq = ( *pulSeq );
    xFrame.lValue = ( *plValue );
    xFrame.ulTick = xTaskGetTickCount();

    /* xQueueOverwrite:队列满(容量 1)时丢最旧帧写新帧,绝不阻塞。 */
    xQueueOverwrite( xSampleQueue, &xFrame );

    /* [09] 投完帧,顺手 set 一下 BIT_NEW_SAMPLE,告诉 display「队列里有新货了」。
     * display 在 xBootEvents 上做「初始化完成 AND 有新采样」的组合等待,这一 set 就是
     * 满足它 AND 条件里的「新采样」那一半。set 位是任务上下文的安全操作,不用 FromISR。 */
    xEventGroupSetBits( xBootEvents, BIT_NEW_SAMPLE );

    console_print( "sensor: sample #%lu value=%ld (%s, mode=%s) sent to queue\n",
                   ( unsigned long ) ( *pulSeq ),
                   ( long ) ( *plValue ),
                   fFromButton ? "BUTTON! extra sample" : "periodic",
                   g_ulSampleMode == MODE_BOOST ? "BOOST" : "normal" );

    /* [08] 在临界区里更新共享统计计数。整段「读改写」用互斥量包起来,保证不被
     * 抢在中间导致丢更新。这里 take 不带超时(portMAX_DELAY):计数更新很短、不会
     * 长期持锁,等一下没关系;真要写「绝不阻塞」的路径(如 ISR)才需要换 FromISR 版。 */
    xSemaphoreTake( xStatsMutex, portMAX_DELAY );
    {
        g_xStats.ulTotalSamples++;
        if( fFromButton )
        {
            g_xStats.ulButtonSamples++;
        }
    }
    xSemaphoreGive( xStatsMutex );

    ( *pulSeq )++;
    ( *plValue )++;
}

/* [06/07] 周期采样滴答回调:跑在定时器服务任务上下文,扮演「采样滴答 ISR」。
 * 只做极短的 give 一行,绝不阻塞、不干长活——真正的采样交给 sensor 任务。
 * [07] 这里从 [06] 的 xTaskNotifyGive 改成了 xSemaphoreGiveFromISR,统一用信号量
 * 把「滴答」事件从类 ISR 上下文递交到 sensor 任务,和按钮事件叙事一致。 */
static void prvSampleTimerCallback( TimerHandle_t xTimer )
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    ( void ) xTimer;

    xSemaphoreGiveFromISR( xSampleSemaphore, &xHigherPriorityTaskWoken );
    /* 注意:这里没调 portYIELD_FROM_ISR。POSIX 移植下实测在定时器服务任务上下文里
     * 叠加 portYIELD_FROM_ISR 会和后续调度打架、偶发 stall;give 完直接返回,
     * 高优先级的 sensor 任务会在下一个调度点被切上(滴答/阻塞超时都行),稳得多。
     * 真硬件 ISR 里 give 后调 portYIELD_FROM_ISR 是标准且正确的写法,这里省略纯粹是
     * host 模拟的稳妥取舍,不是范本。 */
    ( void ) xHigherPriorityTaskWoken;
}

/* [07] 「按钮源」回调:跑在定时器服务任务上下文,扮演「按钮 ISR」。
 * 这就是本章新增的中断源——它「发生」时给一把二值信号量,把「按钮按下了」这件事
 * 从类 ISR 上下文递交到 sensor 任务。极短、give 即返回。
 * 真硬件上把这个回调换成真按钮的 GPIO ISR,内部这一行 give 一字不改。 */
static void prvButtonCallback( TimerHandle_t xTimer )
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    ( void ) xTimer;

    xSemaphoreGiveFromISR( xButtonSemaphore, &xHigherPriorityTaskWoken );
    ( void ) xHigherPriorityTaskWoken;
}

/* sensor 采集任务:主阻塞在周期滴答信号量上(带短超时),每轮顺手零超时探一次按钮
 * 信号量,再探一次自己的任务通知值(看控制源有没有下发新模式)。
 *   周期滴答来→采「周期帧」;
 *   按钮事件来→额外采一帧标 [BUTTON!];
 *   [10] 任务通知里取到新模式码→切采样模式,后续帧的 mode= 字段跟着变。
 * 用「主信号量短超时 + 旁路信号量零超时 + 旁路任务通知零超时」而非队列集,
 * 是因为实测「定时器服务任务里 FromISR + 队列集等待」在 host 下会 stall。
 * 注意模式切换通道(任务通知)和两把信号量是**三类不同的轻量唤醒手段**,刻意区分:
 * 周期滴答和按钮是无值的(信号量),模式切换是带值的(xTaskNotify 携带模式码)。 */
static void prvSensorTask( void *pvParameters )
{
    ( void )pvParameters;
    uint32_t ulSeq = 0;
    int32_t lValue = 0;

    for( ; ; )
    {
        /* 主阻塞:等周期滴答信号量,带 SAMPLE_POLL_TIMEOUT_MS 超时。
         * 拿到(pdPASS)说明「周期滴答 ISR」刚 give 过,采一帧周期帧。 */
        if( xSemaphoreTake( xSampleSemaphore, pdMS_TO_TICKS( SAMPLE_POLL_TIMEOUT_MS ) ) == pdPASS )
        {
            prvTakeAndEnqueueSample( &ulSeq, &lValue, false );
        }

        /* 旁路 1:零超时探一下按钮信号量,有(==pdPASS)就额外采一帧标 [BUTTON!]。
         * 零超时意味着「没有按钮事件就立刻返回 pdFAIL」,不阻塞——这一探的开销极小。
         * 由于主阻塞每 SAMPLE_POLL_TIMEOUT_MS 就醒来一次,按钮事件最长延迟不超过
         * 这个窗口,既够及时、又不依赖(不稳的)队列集。 */
        if( xSemaphoreTake( xButtonSemaphore, 0 ) == pdPASS )
        {
            prvTakeAndEnqueueSample( &ulSeq, &lValue, true );
        }

        /* [10] 旁路 2:零超时探一下「自己的」任务通知值,看控制源有没有下发新模式码。
         * ulTaskNotifyTake 第一参数 pdTRUE=取走后清零通知值,第二参数 0=零超时(没通知就
         * 立刻返回 0,不阻塞)。返回非 0 即「控制源刚 xTaskNotify 过、带了个模式码」;
         * 返回 0 即「没有新指令」,维持当前 g_ulSampleMode 不变。
         *
         * 这里把任务通知当「带值的控制通道」用,正是它区别于上面那两把信号量的地方:
         * 信号量只能 give/take 一下、带不了码;任务通知把 MODE_NORMAL/MODE_BOOST 这个
         * 码直接写进通知值,一次 xTaskNotify 就把「该切模式了 + 切到哪种」一起送达。
         * sensor 此前的通知值一直空着,正好不和其他通道抢资源。 */
        uint32_t ulNewMode = ulTaskNotifyTake( pdTRUE, 0 );
        if( ulNewMode == MODE_NORMAL || ulNewMode == MODE_BOOST )
        {
            if( ulNewMode != g_ulSampleMode )
            {
                g_ulSampleMode = ulNewMode;
                console_print( "sensor: mode switch via notify value=%lu -> %s\n",
                               ( unsigned long ) ulNewMode,
                               g_ulSampleMode == MODE_BOOST ? "BOOST" : "normal" );
            }
        }
    }
}

/* [10] 控制源任务:周期性地用 xTaskNotify 给 sensor 下发「采样模式」切换指令。
 * 这是本章新增的「带值的任务通知」发送侧——和 [06] 的 xTaskNotifyGive(无值)、[07] 的
 * xSemaphoreGiveFromISR(无值信号量)刻意区分,演示任务通知能携带一个 32 位值。
 *
 * 下发策略:normal / boost 来回切,每 CONTROL_PERIOD_MS 切一次。用 eSetValueWithOverwrite
 * 覆盖式写通知值:如果 sensor 还没来得及消费上一条、控制源又下了一条,新的覆盖旧的,
 * sensor 永远取到「最新模式」——这正是「最新值」语义,适合控制类通信(不积压、不掉队)。 */
static void prvControlTask( void *pvParameters )
{
    ( void ) pvParameters;
    uint32_t ulMode = MODE_BOOST;    /* 第一次切的模式,下一轮先变 BOOST */

    for( ; ; )
    {
        vTaskDelay( pdMS_TO_TICKS( CONTROL_PERIOD_MS ) );

        /* normal ↔ boost 来回切。 */
        ulMode = ( ulMode == MODE_BOOST ) ? MODE_NORMAL : MODE_BOOST;

        /* xTaskNotify:给 sensor 任务发通知,把 ulMode 覆盖式写进它的通知值。
         *   参数 1:目标任务句柄(任务通知是定向的);
         *   参数 2:要写的 32 位值(模式码);
         *   参数 3:eSetValueWithOverwrite——直接覆盖通知值。 */
        xTaskNotify( xSensorTaskHandle, ulMode, eSetValueWithOverwrite );

        console_print( "control: sent notify value=%lu (%s)\n",
                       ( unsigned long ) ulMode,
                       ulMode == MODE_BOOST ? "BOOST" : "normal" );
    }
}

/* [05] display 显示任务:在采样队列上阻塞读。队列空就 blocked 等到 sensor 投递。 */
/* [05/09] display 显示任务。
 * [05] 原本在采样队列上阻塞读:队列空就 blocked 等到 sensor 投递。
 * [09] 现在改成「组合门」:在动手显示一帧之前,先用 xEventGroupWaitBits 等「初始化
 * 完成 AND 有新采样」两个位都置位(AND 等待),只有都满足才往下走。这是本章的演示点——
 * 事件组最擅长的「等多事件组合」。等到了之后手动 ClearBits 清掉 BIT_NEW_SAMPLE(不清
 * BIT_INIT_DONE,让它当常驻门),再从队列取帧显示。 */
static void prvDisplayTask( void *pvParameters )
{
    ( void )pvParameters;
    uint32_t ulLastShown = 0;

    for( ; ; )
    {
        /* [09] AND 组合等待:BIT_INIT_DONE 和 BIT_NEW_SAMPLE 两个位「都」置位才解除阻塞。
         *   waitAll = pdTRUE —— 这就是 AND;若要 OR(任一即可)给 pdFALSE。
         *   clearOnExit = pdFALSE —— 故意不清,我们想在下面手动只清 BIT_NEW_SAMPLE、
         *     保留 BIT_INIT_DONE 这道常驻门。xEventGroupWaitBits 的 clearOnExit 是「一刀切
         *     清掉所有等到的位」,要做精细的「只清这个、不清那个」就得手动 ClearBits。
         * portMAX_DELAY 死等:初始化没完成、或队列还没新帧,就老老实实 blocked、不占 CPU。 */
        ( void ) xEventGroupWaitBits( xBootEvents,
                                      BIT_INIT_DONE | BIT_NEW_SAMPLE,
                                      pdFALSE,    /* 不在这里清,手动清 */
                                      pdTRUE,     /* AND:两个位都到位才返回 */
                                      portMAX_DELAY );

        /* 消费掉「新采样」标记:这一帧的 BIT_NEW_SAMPLE 我要吃了,清掉;BIT_INIT_DONE 不动。 */
        xEventGroupClearBits( xBootEvents, BIT_NEW_SAMPLE );

        /* 队列里此刻一定有一帧(sensor 投帧时才 set 的 BIT_NEW_SAMPLE,一一对应)。
         * 用零超时取:取不到(pdFAIL)说明状态不一致(理论上不该发生),那就跳过这轮。 */
        SampleFrame_t xFrame;
        BaseType_t xGot = xQueueReceive( xSampleQueue, &xFrame, 0 );

        if( xGot == pdPASS )
        {
            console_print( "display: showing sample #%lu value=%ld (sampled %lu ms ago, prev #%lu)\n",
                           ( unsigned long ) xFrame.ulSeq,
                           ( long ) xFrame.lValue,
                           ( unsigned long ) ( xTaskGetTickCount() - xFrame.ulTick ),
                           ( unsigned long ) ulLastShown );
            ulLastShown = xFrame.ulSeq;

            /* [08] 显示一帧后,在临界区里把「总显示数」+1。和 sensor 侧用的是同一把
             * xStatsMutex,所以 display 和 sensor 对这组计数器的写永远互斥、不会丢更新。 */
            xSemaphoreTake( xStatsMutex, portMAX_DELAY );
            g_xStats.ulTotalDisplays++;
            xSemaphoreGive( xStatsMutex );
        }
    }
}

/* [09] 初始化任务:延时 INIT_DONE_DELAY_MS 模拟「系统初始化要花点时间」,然后 set
 * BIT_INIT_DONE,把 display 的 AND 组合等待解锁。set 完自删。这一位之后从不清——它就是
 * display 那道「初始化已完成」的常驻门,每次组合等待都要求它在位。 */
static void prvInitTask( void *pvParameters )
{
    ( void ) pvParameters;
    console_print( "init: system initialization started\n" );
    vTaskDelay( pdMS_TO_TICKS( INIT_DONE_DELAY_MS ) );
    console_print( "init: initialization done, set BIT_INIT_DONE\n" );
    xEventGroupSetBits( xBootEvents, BIT_INIT_DONE );
    vTaskDelete( NULL );
}

/* [08] 统计任务:周期性地「读一份快照」并打印。读者也要持锁——拿到锁后把整组计数器
 * 拷到局部变量、立刻 give,然后在锁外慢慢格式化打印。这样临界区只覆盖「拷三个字」
 * 这么短,绝不把 console_print(慢 I/O)关在锁里;同时拿到的快照是一组彼此一致的值。 */
static void prvStatsTask( void *pvParameters )
{
    ( void )pvParameters;

    for( ; ; )
    {
        vTaskDelay( pdMS_TO_TICKS( STATS_PERIOD_MS ) );

        StatsCounters_t xSnapshot;
        xSemaphoreTake( xStatsMutex, portMAX_DELAY );
        xSnapshot = g_xStats;    /* 整组拷走,锁只护这一下 */
        xSemaphoreGive( xStatsMutex );

        console_print( "stats: samples=%lu (buttons=%lu)  displays=%lu\n",
                       ( unsigned long ) xSnapshot.ulTotalSamples,
                       ( unsigned long ) xSnapshot.ulButtonSamples,
                       ( unsigned long ) xSnapshot.ulTotalDisplays );
    }
}

/* [12] 本章增量:加一个运行时统计(CPU 占用)周期输出任务。
 * 起因是个很自然的诉求:dashboard 里现在有 Sensor / Display / HeapMon / Stats / Init /
 * Control 一堆任务(外加内核的 idle / timer daemon),它们各自吃多少 CPU?这正好演
 * configGENERATE_RUN_TIME_STATS 的看家本领——各任务的 CPU 占用百分比。
 *
 * 实现要点:
 *   - POSIX port 已替我们把「运行时统计时钟」接好了(portGET_RUN_TIME_COUNTER_VALUE 映射到
 *     ulPortGetRunTime,portCONFIGURE_TIMER_FOR_RUN_TIME_STATS 是 no-op),所以我们只要
 *     开了 configGENERATE_RUN_TIME_STATS(模板=1),就能直接拿数。
 *   - 用 uxTaskGetSystemState 一次性拍下所有任务的 TaskStatus_t(含 ulRunTimeCounter),
 *     顺带拿到总运行时 *pulTotalRunTime,占比 = ulRunTimeCounter*100/total。
 *   - 这个任务优先级压到最低(同 Stats/HeapMon 一档),只在「没有正经任务想跑」时插进来;
 *     否则它自己拍快照、做格式化打印这段 CPU 时间会被记进统计、把数据污染了。
 *   - 起手先睡一拍 STATS_PERIOD_MS,让前面那些任务先跑起来、积累一段运行时统计,
 *     否则第一张表全是 0% 没看头。
 * 它呼应 [04] 那张「状态快照」(那是 state/prio/freestack),这里补的是「CPU 占用」那一列,
 * 把 dashboard 的可观测性又往前推了一格。 */
#define CPU_STATS_TASK_PRIO     ( tskIDLE_PRIORITY + 1 )
#define CPU_STATS_PERIOD_MS    ( 4000 )
#define CPU_STATS_MAX_TASKS    ( 16 )

/* eTaskState 是个枚举,值是数字,打印时翻译成人话。 */
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

static void prvCpuStatsTask( void *pvParameters )
{
    ( void )pvParameters;
    static TaskStatus_t xStatus[ CPU_STATS_MAX_TASKS ];

    /* 先睡一拍,让别的任务先跑起来、积累一段运行时统计,否则第一张表全是 0%。 */
    vTaskDelay( pdMS_TO_TICKS( CPU_STATS_PERIOD_MS ) );

    for( ; ; )
    {
        configRUN_TIME_COUNTER_TYPE ulTotalRunTime = 0;
        UBaseType_t uxCount = uxTaskGetSystemState( xStatus, CPU_STATS_MAX_TASKS, &ulTotalRunTime );

        console_print( "---- cpu stats @ tick %lu (configGENERATE_RUN_TIME_STATS=1) ----\n",
                       ( unsigned long ) xTaskGetTickCount() );

        if( uxCount == 0 || ulTotalRunTime == 0 )
        {
            console_print( "  (no run-time stats yet)\n" );
        }
        else
        {
            /* 按运行时占用降序排,最吃 CPU 的排最上面,读起来直观。任务数很少,O(n^2) 无所谓。 */
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

            for( UBaseType_t i = 0; i < uxCount; i++ )
            {
                /* 占用 % = ulRunTimeCounter × 100 / 总运行时。占用不足 1% 的标 <1%。
                 * freestack 单位是「字(StackType_t)」不是字节,和 [04] 状态快照里一样。 */
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
                console_print( "  %-12s %-8s prio=%lu freestack=%lu  CPU=%s\n",
                               xStatus[ i ].pcTaskName,
                               prvStateName( xStatus[ i ].eCurrentState ),
                               ( unsigned long ) xStatus[ i ].uxCurrentPriority,
                               ( unsigned long ) xStatus[ i ].usStackHighWaterMark,
                               cPct );
            }
        }
        console_print( "--------------------------------------------------------\n" );

        vTaskDelay( pdMS_TO_TICKS( CPU_STATS_PERIOD_MS ) );
    }
}

/* [03] 内存余量监控任务:每 2 秒探一次堆。 */
static void prvHeapMonitorTask( void *pvParameters )
{
    ( void )pvParameters;

    for( ; ; )
    {
        console_print( "heap monitor: free=%lu  min_ever=%lu\n",
                       ( unsigned long ) xPortGetFreeHeapSize(),
                       ( unsigned long ) xPortGetMinimumEverFreeHeapSize() );
        vTaskDelay( pdMS_TO_TICKS( 2000 ) );
    }
}

int main( void )
{
    /* 关掉 stdout 全缓冲,避免重定向/管道时输出丢失(见 02_environment 排坑章)。 */
    setvbuf( stdout, NULL, _IOLBF, 0 );

    console_init();

    /* [05] 采样队列必须在进 scheduler 之前创建好。容量 1、每帧一个 SampleFrame_t。 */
    xSampleQueue = xQueueCreate( SAMPLE_QUEUE_LENGTH, sizeof( SampleFrame_t ) );
    configASSERT( xSampleQueue != NULL );

    /* [06/07] 两把唤醒信号量。二值信号量刚创建为「空」,sensor 任务会一直阻塞/轮询
     * 直到对应定时器回调 give。 */
    xSampleSemaphore = xSemaphoreCreateBinary();
    configASSERT( xSampleSemaphore != NULL );

    xButtonSemaphore = xSemaphoreCreateBinary();
    configASSERT( xButtonSemaphore != NULL );

    /* [08] 保护共享统计计数的互斥量。xSemaphoreCreateMutex 创建即「满」、可直接 take,
     * 且内核会做优先级继承。要在进 scheduler 之前建好——sensor/display/stats 一启动就会
     * 拿它,迟建的话它们第一次 take 会撞上 NULL。 */
    xStatsMutex = xSemaphoreCreateMutex();
    configASSERT( xStatsMutex != NULL );

    /* [09] 事件组:display 的「初始化完成 AND 有新采样」组合门就建在这上面。同样要进
     * scheduler 之前建好——sensor 一启动就会 set BIT_NEW_SAMPLE,InitTask 会 set
     * BIT_INIT_DONE,迟建的话它们会撞 NULL。 */
    xBootEvents = xEventGroupCreate();
    configASSERT( xBootEvents != NULL );

    /* [04] sensor 优先级最高,确保采样时序;display 低一档;内存监控最低。 */
    /* [10] sensor 这里拿到句柄 xSensorTaskHandle——控制源任务要靠它 xTaskNotify 给 sensor
     * 下发模式切换指令(任务通知是定向的,必须知道发给谁)。 */
    xTaskCreate( prvSensorTask, "Sensor", configMINIMAL_STACK_SIZE, NULL,
                 prioSENSOR, &xSensorTaskHandle );
    xTaskCreate( prvDisplayTask, "Display", configMINIMAL_STACK_SIZE, NULL,
                 prioDISPLAY, NULL );
    xTaskCreate( prvHeapMonitorTask, "HeapMon", configMINIMAL_STACK_SIZE, NULL,
                 prioHEAP_MONITOR, NULL );
    /* [08] 统计任务:优先级压到最低,和 HeapMon 同档,只在有空时插进来读快照。 */
    xTaskCreate( prvStatsTask, "Stats", configMINIMAL_STACK_SIZE, NULL,
                 prioSTATS, NULL );
    /* [09] 初始化任务:延时后 set BIT_INIT_DONE,解锁 display 的 AND 等待。优先级和
     * sensor 同档,保证到点及时 set,不拖延 display 的解锁。 */
    xTaskCreate( prvInitTask, "Init", configMINIMAL_STACK_SIZE, NULL,
                 prioINIT, NULL );
    /* [10] 控制源任务:周期性 xTaskNotify 给 sensor 下发模式切换指令。优先级压在 sensor
     * 下面一档,只「下一道指令」就退,不和 sensor 抢采样时序。 */
    xTaskCreate( prvControlTask, "Control", configMINIMAL_STACK_SIZE, NULL,
                 prioCONTROL, NULL );
    /* [12] CPU 统计任务:周期打印各任务 CPU 占用(configGENERATE_RUN_TIME_STATS)。
     * 优先级压到最低(同 HeapMon/Stats),只在有空时插进来,免得自己污染统计。 */
    xTaskCreate( prvCpuStatsTask, "CpuStats", configMINIMAL_STACK_SIZE, NULL,
                 CPU_STATS_TASK_PRIO, NULL );

    /* [06] 周期采样定时器:回调 prvSampleTimerCallback give xSampleSemaphore。 */
    xSampleTimer = xTimerCreate( "SampleTmr",
                                 pdMS_TO_TICKS( SAMPLE_TIMER_PERIOD_MS ),
                                 pdTRUE,
                                 NULL,
                                 prvSampleTimerCallback );
    configASSERT( xSampleTimer != NULL );

    BaseType_t xTimerOk = xTimerStart( xSampleTimer, portMAX_DELAY );
    configASSERT( xTimerOk == pdPASS );

    /* [07] 「按钮源」周期定时器:回调 prvButtonCallback give xButtonSemaphore。
     * 它在定时器服务任务上下文里跑,扮演「按钮 ISR」,从这里 give 信号量是 host
     * 模拟下「相对安全地模拟中断源」的标准落点。 */
    xButtonTimer = xTimerCreate( "ButtonTmr",
                                 pdMS_TO_TICKS( BUTTON_TIMER_PERIOD_MS ),
                                 pdTRUE,
                                 NULL,
                                 prvButtonCallback );
    configASSERT( xButtonTimer != NULL );

    xTimerOk = xTimerStart( xButtonTimer, portMAX_DELAY );
    configASSERT( xTimerOk == pdPASS );

    console_print( "dashboard: starting scheduler (heap=%lu bytes, sample=%d ms, button=%d ms, init=%d ms, control=%d ms, cpu_stats=%d ms)\n",
                   ( unsigned long ) configTOTAL_HEAP_SIZE,
                   SAMPLE_TIMER_PERIOD_MS,
                   BUTTON_TIMER_PERIOD_MS,
                   INIT_DONE_DELAY_MS,
                   CONTROL_PERIOD_MS,
                   CPU_STATS_PERIOD_MS );

    vTaskStartScheduler();

    for( ; ; )
    {
    }

    return 0;
}
