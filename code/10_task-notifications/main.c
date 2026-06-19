/*
 * 10_task-notifications —— 任务通知(Task Notification)演示。
 *
 * 前面几章我们把信号量(中断章)、队列(队列章)、事件组(事件组章)都用过了一圈。
 * 这三样原语各有各的位子:信号量传「一个事件」、队列传「一份数据」、事件组传
 * 「一组位的组合」。但有一类很常见的需求,它们三个都显得「重」了——就是**两个
 * 任务之间、单点、一对一、而且最好顺带能塞一点点数据**的通信。比如「通知采集任务
 * 马上再采一帧」「通知显示任务该刷新了」「通知控制任务切换工作模式」。这种场景
 * 用队列?队列要建一个 Queue_t 对象、维护一条链表、send/receive 还各做一次拷贝,
 * 就为了传一句「你该干活了」,太重;用信号量?信号量只能表达「来了一下」、带不了
 * 任何数据(动作码、参数统统塞不进去);用事件组?更重,还得规划位。
 *
 * 任务通知就是为这类「轻量单点通信」量身定做的原语。它的核心想法是:**不另建对象,
 * 把「一个 32 位的通知值 + 一个通知状态」直接塞进每个任务的任务控制块(TCB)里**。
 * 也就是说,任务通知不需要 xTaskNotifyCreate 这种「创建」步骤——每个任务天生就自带
 * 一个通知值,谁有它的句柄谁就能往里写。正因为省掉了独立的同步对象,任务通知在
 * RAM 占用和执行速度上都比信号量/队列/事件组轻一截:省一个对象、省一条等待链表、
 * 唤醒路径更短。官方给的说法是「比信号量快 ~45%」(具体数字看平台,但量级是真的)。
 *
 * 这章我们重点演的是任务通知**能携带一个 32 位值**这件事——这是它区别于信号量
 * 「只能传一个 token」的关键卖点。我们设计这样一个场景:一个「控制源」任务周期性地
 * 给「工作」任务发通知,每次发通知时用那个 32 位通知值携带一个**动作码**(NORMAL /
 * BOOST / SHUTDOWN 三选一);工作任务 ulTaskNotifyTake 取到通知值后,按动作码分支
 * 做不同的事。这一套「通知 + 携带动作码」是信号量做不到的(信号量 give 一下就是
 * 一下、带不了数据),正好把任务通知的独有价值演出来。
 *
 * 同时我们也对照演一把「同样这件事用信号量怎么做」:信号量版本因为带不了动作码,
 * 控制源要表达「切换模式」只能另开一条带外通道(一个全局变量),把数据通道和通知
 * 通道拆成两半——既啰嗦,又天然有「写到变量了但还没 give」「give 了但变量还没读」
 * 之类的竞态窗口。任务通知用「通知值即数据」把这两半合一,干净利落。
 *
 * API 速览(本章用到的):
 *   xTaskNotifyGive(xTask)                —— 给任务发一个「无值」通知:通知值 +1、状态
 *                                            置为「已通知」。(等价于 xTaskNotify 用
 *                                            eIncrement 动作,是给信号量风格场景的快捷写法。)
 *   ulTaskNotifyTake(xClearOnExit, xTicks) —— 配合 xTaskNotifyGive 用:取通知并把值清零
 *                                            (xClearOnExit=pdTRUE)或只减 1(pdFALSE),
 *                                            状态置回「未通知」。当通知值清到 0 就阻塞。
 *   xTaskNotify(xTask, ulValue, eAction)   —— 通用发通知:带一个 32 位 ulValue 和一个
 *                                            「怎么处理这个值」的动作 eAction(见下)。
 *   xTaskNotifyWait(...)                    —— 通用取通知:可以读通知值、按需清位。
 *
 * eAction 的取值决定「那个 32 位值怎么和通知值合并」:
 *   eSetBits        —— 通知值 |= ulValue   (当事件组用)
 *   eIncrement      —— 通知值 += 1         (xTaskNotifyGive 就是这个)
 *   eSetValueWithOverwrite —— 通知值 = ulValue(覆盖;本章用这个传动作码)
 *   eSetValueWithoutOverwrite —— 同上但不覆盖已挂起的通知
 *   eNoAction       —— 只发通知不动值      (纯信号量风格)
 *
 * 本章 demo 的两条任务:
 *   - prvControlTask(控制源):周期性 xTaskNotify 工作任务,每次用一个动作码
 *     (NORMAL/BOOST/SHUTDOWN)当通知值,eSetValueWithOverwrite 覆盖式下发。
 *   - prvWorkerTask(工作任务):ulTaskNotifyTake 死等通知,取到值就按动作码分支干活。
 *
 * 环境说明:POSIX host 模拟(heap_4 + 2MB,见 CMakeLists/FreeRTOSConfig.h,沿用
 * dashboard 已踩平的配置)。所有 FreeRTOS 必需的回调样板在 app_hooks.c,本文件
 * 只放 demo 逻辑。
 *
 * 教学边界(诚实交代):任务通知是纯软件机制,host 模拟和真硬件行为完全一致——
 * 通知值、通知状态、各 eAction 语义、阻塞唤醒,这套东西不存在「host 模拟不真实」
 * 的问题。它也有 *FromISR 版本(xTaskNotifyFromISR / vTaskNotifyGiveFromISR),
 * 用于中断里给任务发通知(中断章讲过 FromISR 的规矩,这里同理);本章 demo 全程
 * 在任务上下文里发通知,不涉及 ISR。
 */

#include <stdbool.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "console.h"

/* 工作任务优先级:故意给到较高档(+4),让控制源一发通知、它一就绪就被及时调度,
 * 这样「控制源 notify → 工作任务醒来干活」的链路在输出里紧凑可读。 */
#define prioWORKER    ( tskIDLE_PRIORITY + 4 )
/* 控制源优先级:比工作任务低一档——它只负责定时「下一道指令」,真正干活的是工作任务。 */
#define prioCONTROL   ( tskIDLE_PRIORITY + 3 )

/* 控制源下发指令的周期。每 600ms 切换一次动作,纯属让输出一行行整齐、便于对照。 */
#define CONTROL_PERIOD_MS    ( 600 )

/* 「控制源」下发的动作码。这正是任务通知能携带「一个 32 位值」的用武之地——
 * 信号量做不到带这个码,只能 give 一下;任务通知把这个码直接塞进通知值一路传过去。
 * 我们用一组小整数当动作码,工作任务取到后 switch 分支处理。 */
#define ACTION_NORMAL     ( 100 )    /* 普通模式:照常采一帧 */
#define ACTION_BOOST      ( 200 )    /* 加速模式:标记一下、多采一帧 */
#define ACTION_SHUTDOWN   ( 999 )    /* 关机:工作任务自删退出 */

/* 工作任务句柄:控制源需要它来 xTaskNotify(任务通知是「发给指定任务」的,
 * 不像信号量有一个独立对象谁都能 give——这是任务通知「一对一、定向」的本质)。 */
static TaskHandle_t xWorkerTaskHandle = NULL;

/* 工作任务:在「自己的通知值」上阻塞,被通知就取值、按动作码分支干活。
 *
 * 这一段是本章的核心——它把「任务通知能携带一个 32 位值」演活了。注意 ulTaskNotifyTake
 * 取出来的是那个 32 位通知值本身(不是「有没有通知」这种布尔):控制源用
 * eSetValueWithOverwrite 把动作码写进去,这里直接读出来 switch,「通知」和「数据」
 * 是同一个值、同一次原子操作。 */
static void prvWorkerTask( void *pvParameters )
{
    ( void ) pvParameters;

    for( ; ; )
    {
        /* ulTaskNotifyTake:在工作任务「自己的」通知上阻塞等待(不需要句柄,
         * 因为每个任务天生有一个通知)。第一参数 xClearOnExit=pdTRUE:取到值后把通知值
         * 清零(我们是「每次一个动作码」语义,取走就清);第二参数 portMAX_DELAY:死等,
         * 没通知就 blocked、不占 CPU。
         *
         * 这里和 xTaskNotifyGive 的「轻量信号量」用法有一点细微差别:那套用法常配
         * xClearOnExit=pdFALSE(逐个 -1,支持「积压计数」);本 demo 每次 eSetValueWithOverwrite
         * 覆盖整个值,不存在积压概念,所以取走就清零。 */
        uint32_t ulAction = ulTaskNotifyTake( pdTRUE, portMAX_DELAY );

        /* 取到的 ulAction 就是控制源塞进来的动作码——这就是任务通知「带数据」的现场。
         * 按动作码分支干活。 */
        switch( ulAction )
        {
            case ACTION_NORMAL:
                console_print( "[worker] notify value=%lu -> NORMAL: take a frame\n",
                               ( unsigned long ) ulAction );
                break;

            case ACTION_BOOST:
                /* BOOST 模式:这里我们只是标记一下「这帧来自 boost」。任务通知的价值
                 * 在于控制源无需另一条带外通道、无需额外信号量,一个动作码就把
                 * 「该干嘛」一次性说清了。 */
                console_print( "[worker] notify value=%lu -> BOOST: high-priority frame!\n",
                               ( unsigned long ) ulAction );
                break;

            case ACTION_SHUTDOWN:
                console_print( "[worker] notify value=%lu -> SHUTDOWN: exiting\n",
                               ( unsigned long ) ulAction );
                /* 收到关机指令:删掉自己。任务函数里 vTaskDelete(NULL) 即「删我自己」。 */
                vTaskDelete( NULL );
                /* 不会走到这里,仅为让编译器满意。 */
                break;

            default:
                /* 通知值既可能是控制源没下发过时的初值(0),也可能是其他未识别码。
                 * 初值 0 的情况在第一次循环理论上不会出现(ulTaskNotifyTake 会一直阻塞
                 * 到有通知),这里 default 兜个底、不打断流程。 */
                console_print( "[worker] notify value=%lu -> unknown action, ignore\n",
                               ( unsigned long ) ulAction );
                break;
        }
    }
}

/* 控制源任务:周期性下发动作码。它用 xTaskNotify 的 eSetValueWithOverwrite 动作
 * 把动作码覆盖式写进工作任务的通知值——这是「任务通知带数据」的发送侧。
 *
 * eSetValueWithOverwrite 的含义:无论工作任务当前通知值有没有被取走,都把通知值直接
 * 设成 ulValue。在我们这套「控制源单向、定时下指令、工作任务单向消费」的模型里,
 * 覆盖语义正合适:如果工作任务还没来得及消费上一条、控制源又下了一条,新的覆盖旧的、
 * 工作任务取到的永远是「最新指令」——这是「最新值」语义,和队列的「积压所有消息」
 * 语义相反,恰好是控制类通信想要的。
 *
 * 下发序列:NORMAL → NORMAL → BOOST → NORMAL → SHUTDOWN,把三种动作码都过一遍、
 * 最后用 SHUTDOWN 让工作任务干净退出、demo 自然收尾。 */
static void prvControlTask( void *pvParameters )
{
    ( void ) pvParameters;

    /* 下发序列表。控制源按这个顺序逐条下发,下完即停。 */
    static const uint32_t ulSequence[] =
    {
        ACTION_NORMAL,
        ACTION_NORMAL,
        ACTION_BOOST,
        ACTION_NORMAL,
        ACTION_SHUTDOWN,
    };
    const size_t uxSeqLen = sizeof( ulSequence ) / sizeof( ulSequence[ 0 ] );

    console_print( "[control] starting, will send %zu notifications\n", uxSeqLen );

    for( size_t i = 0; i < uxSeqLen; i++ )
    {
        uint32_t ulAction = ulSequence[ i ];

        /* xTaskNotify:给 xWorkerTaskHandle 发一个通知,把 ulAction 覆盖式写进它的通知值。
         *   参数 1:目标任务句柄(任务通知是定向的,必须知道发给谁);
         *   参数 2:要写的 32 位值(这里是动作码);
         *   参数 3:eAction,eSetValueWithOverwrite 表示「直接覆盖通知值」。
         * 返回值 pdPASS 表示成功;若用 eSetValueWithoutOverwrite 而目标任务有未取走的
         * 通知,会返回 pdFAIL(本 demo 用 Overwrite,恒成功)。 */
        xTaskNotify( xWorkerTaskHandle, ulAction, eSetValueWithOverwrite );

        console_print( "[control] sent notify, action=%lu (%s)\n",
                       ( unsigned long ) ulAction,
                       ulAction == ACTION_NORMAL ? "NORMAL" :
                       ulAction == ACTION_BOOST ? "BOOST" : "SHUTDOWN" );

        vTaskDelay( pdMS_TO_TICKS( CONTROL_PERIOD_MS ) );
    }

    /* 下发完序列,控制源任务也自删退出。此时只剩(已收到 SHUTDOWN 自删的)工作任务之外
     * 的空闲任务,demo 自然停止产生输出。 */
    console_print( "[control] sequence done, control task exiting\n" );
    vTaskDelete( NULL );
}

int main( void )
{
    /* 关掉 stdout 全缓冲,避免重定向/管道时输出丢失(见 02_environment 排错章)。 */
    setvbuf( stdout, NULL, _IOLBF, 0 );

    console_init();

    /* 先建工作任务,拿到它的句柄——控制源需要这个句柄才能 xTaskNotify 给它。
     * 注意:任务通知不需要任何「创建」步骤(没有 xTaskNotifyCreate),句柄就是 xTaskCreate
     * 的出参。这也是它比信号量/队列省一截的根源:省掉了一个独立的同步对象。 */
    xTaskCreate( prvWorkerTask, "Worker", configMINIMAL_STACK_SIZE, NULL,
                 prioWORKER, &xWorkerTaskHandle );
    configASSERT( xWorkerTaskHandle != NULL );

    /* 控制源任务:优先级比工作任务低一档,定时下发动作码。 */
    xTaskCreate( prvControlTask, "Control", configMINIMAL_STACK_SIZE, NULL,
                 prioCONTROL, NULL );

    console_print( "10_task-notifications: notify-with-value demo "
                   "(NORMAL=%d BOOST=%d SHUTDOWN=%d, period=%d ms)\n",
                   ACTION_NORMAL, ACTION_BOOST, ACTION_SHUTDOWN,
                   CONTROL_PERIOD_MS );

    vTaskStartScheduler();

    /* 调度器正常情况下不会返回;走到这里说明堆不够建 idle/timer 任务,属致命错误。 */
    for( ; ; )
    {
    }

    return 0;
}
