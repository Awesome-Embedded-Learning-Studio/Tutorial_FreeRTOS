/*
 * 07_interrupts —— 中断管理独立 demo:延迟中断处理(deferred interrupt handling)。
 *
 * 这章讲的是 RTOS 里最「贴近硬件」的一块:中断服务程序(ISR)和任务怎么打交道。
 * 真硬件上一个 ISR 是「异步打断 CPU、必须极快返回」的硬件上下文,它和普通任务
 * 之间最经典、也最安全的协作模式叫「延迟中断处理」——ISR 里不干重活,只「通知
 * 一声」(给信号量),真正的活交给一个处理任务(handler task)在信号量上阻塞等、
 * 被唤醒后再去做。这样做有两个好处:一是 ISR 尽快返回、把中断延迟压到最低
 * (长时间占用中断上下文是嵌入式系统的大忌);二是处理任务跑在普通任务上下文,
 * 可以放心地用任何 API(阻塞、拿队列、调 printf),没有 ISR 的种种限制。
 *
 * 本 demo 就是这套模式的完整演练:我们模拟一个周期性的「中断源」,它每 500ms
 * 「发生一次」;发生时只在「类 ISR 上下文」里给一把二值信号量、然后立刻返回;
 * 另一边一个处理任务在信号量上死等,被唤醒就去「干活」(打印一行、报一下延迟),
 * 然后回到阻塞等下一次。我们借此把 ISR 的限制、二值信号量、FromISR 风格 API、
 * portYIELD_FROM_ISR 这一整套概念都演一遍。
 *
 * ⚠️ 必须先讲清的 POSIX 边界(文档里有专节展开):
 *   真硬件上 ISR 是一段在硬件中断上下文里执行的函数,它直接调 xSemaphoreGiveFromISR。
 *   但 FreeRTOS 的 POSIX port 把任务映射成 pthread,调度靠信号在 pthread 间做切换;
 *   这个 port 的 FromISR 实现假设自己「正处在一次被 port 正确跟踪的中断里」,
 *   如果你从一段它不认识的上下文——比如你自己开的 pthread——里直接调 FromISR,
 *   port 内部的中断嵌套计数 / 信号调度会错乱,直接死锁(hang)。
 *
 *   所以 host 模拟下我们不能真「发一个中断」。本 demo 用一个**安全的方式**模拟
 *   中断源:让中断源的「触发」发生在**软件定时器回调**里。软件定时器回调跑在
 *   FreeRTOS 自动建的「定时器服务任务」(daemon task)上下文里——它是一个正经的
 *   FreeRTOS 任务、被 port 正确跟踪——从这里调 FromISR 风格 API,port 不会错乱,
 *   不会 hang。换句话说,定时器回调在这里「扮演」真硬件上 ISR 的角色:它扮演
 *   「中断一发生就被调到、必须极快返回」的那个上下文。
 *
 *   但这里要诚实:这**不是真中断**。host 模拟下没有真 ISR、没有硬件抢占,定时器
 *   回调是被「定时器服务任务」依次调度出来的,它的「即时性」受服务任务调度影响,
 *   不具备真硬件 ISR 的确定性延迟。我们演示的是「延迟中断处理」的**模式与信号量
 *   用法**,这套模式搬到真硬件上,只要把「定时器回调里的 xSemaphoreGiveFromISR」
 *   换成「真 ISR 里的 xSemaphoreGiveFromISR」,其余一行不用改。这个边界、以及
 *   「为什么不能从外部 pthread 直调 FromISR」的死锁机制,见文档和仿真坑点章。
 *
 * 所有 FreeRTOS 必需的回调样板(vAssertCalled / 各类 hook / 静态分配内存)
 * 都在 app_hooks.c 里,这个文件只放 demo 本身的逻辑。CMakeLists 已把
 * FREERTOS_HEAP 选成 4、configTOTAL_HEAP_SIZE 抬到 2MB,和前几章一致。
 */

#include <stdio.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "timers.h"

#include "console.h"

/* 处理任务优先级。configMAX_PRIORITIES=7,合法优先级 0..6。
 * 故意给到接近最高档(+4),模拟真硬件上「被 ISR 唤醒的处理任务应尽快被调度」
 * 的常态——ISR 把活丢给它后,我们希望它尽快抢到 CPU 把活干了。 */
#define prioHANDLER    ( tskIDLE_PRIORITY + 4 )

/* 模拟「中断源」的触发周期。真硬件上中断是异步、随机发生的;这里用一个 500ms
 * 的周期软件定时器来「定时地」模拟中断发生,纯属为了输出好看、便于对照。
 * 换成单次定时器也行,周期只是为了 demo 输出一行行整齐。 */
#define IRQ_SOURCE_PERIOD_MS    ( 500 )

/* 二值信号量句柄。进 scheduler 之前创建好,模拟中断源(定时器回调)「give」它、
 * 处理任务「take」它,二者就靠这一把信号量把「中断发生」这件事从类 ISR 上下文
 * 递交到处理任务。 */
static SemaphoreHandle_t xIrqSemaphore = NULL;

/* 定时器句柄。用周期软件定时器模拟「中断源」。 */
static TimerHandle_t xIrqSourceTimer = NULL;

/* 处理任务句柄:给 portYIELD_FROM_ISR 用(见下)。 */
static TaskHandle_t xHandlerTaskHandle = NULL;

/* 模拟中断源的「类 ISR」回调:跑在定时器服务任务上下文。
 *
 * 这是本章的核心道具——它在 demo 里**扮演真硬件 ISR 的角色**:被「中断发生」
 * 这个事件调到、必须极快返回。所以它的写法严格遵守 ISR 的规矩:
 *   1) 绝不阻塞、不干长活(和真 ISR 一模一样的要求)。
 *   2) 只用 *FromISR 版本的 API(这是 ISR 的硬性限制,见文档)。
 *   3) give 信号量后,若需要让处理任务「立刻」被调度,调 portYIELD_FROM_ISR。
 *
 * 这三件事正是真硬件 ISR 的标准写法,搬过去一行不用改;唯一的差别是「在 host
 * 模拟下,这个上下文是定时器服务任务而不是真硬件中断」,这个差别不会影响代码、
 * 只影响「即时性」(host 下受服务任务调度制约,不实时)。 */
static void prvSimulatedIsr( TimerHandle_t xTimer )
{
    static BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    ( void ) xTimer;

    /* xSemaphoreGiveFromISR 的第二参数是一个「是否需要做上下文切换」的出参:
     * 如果这次 give 唤醒了一个比当前任务更高优先级的任务,它会被写成 pdTRUE。
     * 每次进来都要重新初始化成 pdFALSE,这是 FromISR API 的固定写法。 */
    xHigherPriorityTaskWoken = pdFALSE;

    /* 这就是「中断一发生、给信号量」这一下。处理任务若正阻塞在 xSemaphoreTake
     * 上,这一 give 就把它唤醒(从类 ISR 上下文递交到处理任务上下文)。
     *
     * 注意这里用的是 *FromISR 版本:ISR 上下文只能调带 FromISR 后缀的 API,
     * 不能调普通的 xSemaphoreGive。原因在于普通版本会查「当前在不在任务上下文」、
     * 会做任务切换调度,这些动作在 ISR 里既不安全、也无意义;FromISR 版本为
     * 中断上下文专门做了「不查上下文、把切换请求记下来交给 woken 出参」的处理。
     * 文档里有专节展开这条限制。 */
    xSemaphoreGiveFromISR( xIrqSemaphore, &xHigherPriorityTaskWoken );

    /* portYIELD_FROM_ISR:如果刚那次 give 唤醒了一个更高优先级的任务,这里请求
     * 一次「中断退出时切到它」。host 模拟下这个切换受 pthread 调度制约、不是
     * 真硬件那种中断退出即切换,但语义对齐:处理任务优先级高于服务任务,所以
     * give 完它理应尽快被调度,portYIELD_FROM_ISR 就是表达这个意图。
     * 参数写成 if(woken) yield 是标准写法,内核会自己判断是否真要切。 */
    portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
}

/* 处理任务(handler task):在二值信号量上阻塞死等。
 *
 * 这是延迟中断处理的「任务」那一半:它平时 blocked、不占 CPU;一旦类 ISR
 * (定时器回调)give 了信号量,它就被唤醒,然后在**普通任务上下文**里干真正的活。
 * 关键就这最后一点——因为它在任务上下文里,所以可以放心地用任何 API:阻塞、
 * 队列、printf,统统没有 ISR 的限制。这正是「延迟」二字的精髓:把「不能在 ISR
 * 里干的活」推迟到任务里干。 */
static void prvHandlerTask( void *pvParameters )
{
    ( void )pvParameters;

    for( ; ; )
    {
        /* 在信号量上死等。被唤醒意味着「模拟的中断刚发生」。
         * 注意这里是 xSemaphoreTake(任务上下文版),不是 *FromISR 版——
         * take 在任务里、give(FromISR)在类 ISR 里,这是中断↔任务同步的标配分工。 */
        xSemaphoreTake( xIrqSemaphore, portMAX_DELAY );

        /* 到这里就是「被中断唤醒、开始干延迟了的活」。我们打个时间戳前缀,
         * 再报一下「从这次 give 到真正进入这段处理」的延迟,用来直观感受
         * host 模拟下「延迟中断处理」的即时性(理论上接近 0,实际受调度抖动)。
         *
         * 这些活(打时间戳、格式化打印)如果放进真 ISR 里是不合适的(ISR 里要尽量
         * 短、尽量别 printf);正因为我们把它们推迟到了这个任务里,才毫无顾忌。
         * 这就是延迟中断处理的价值现场。 */
        TickType_t xNow = xTaskGetTickCount();
        console_print( "[handler] woken by simulated IRQ, doing deferred work at tick %lu\n",
                       ( unsigned long ) xNow );
    }
}

int main( void )
{
    /* 关掉 stdout 全缓冲,避免重定向/管道时输出丢失(见 02_environment 排坑章)。 */
    setvbuf( stdout, NULL, _IOLBF, 0 );

    console_init();

    /* 创建二值信号量。xSemaphoreCreateBinary 返回一个初始为「空」的二值信号量
     * (刚创建出来没人 give,处理任务会一直阻塞,直到第一次中断「发生」才被唤醒)
     * —— 这正是我们要的:任务先就位、等着中断来叫它。返回 NULL 是堆不够的致命错,
     * 直接断言。 */
    xIrqSemaphore = xSemaphoreCreateBinary();
    configASSERT( xIrqSemaphore != NULL );

    /* 创建处理任务,拿到句柄(portYIELD_FROM_ISR 路径用,这里其实 give/yield 不
     * 直接需要句柄——信号量本身就知道唤醒谁;保留句柄是为了清晰和后续可扩展)。 */
    xTaskCreate( prvHandlerTask, "Handler", configMINIMAL_STACK_SIZE, NULL,
                 prioHANDLER, &xHandlerTaskHandle );
    ( void ) xHandlerTaskHandle;

    /* 创建并启动「模拟中断源」的周期软件定时器。它到期时回调 prvSimulatedIsr
     * 在定时器服务任务上下文里跑,扮演「中断发生」。周期 500ms 纯为输出好看。 */
    xIrqSourceTimer = xTimerCreate( "IrqSrc",
                                    pdMS_TO_TICKS( IRQ_SOURCE_PERIOD_MS ),
                                    pdTRUE,
                                    NULL,
                                    prvSimulatedIsr );
    configASSERT( xIrqSourceTimer != NULL );

    BaseType_t xTimerOk = xTimerStart( xIrqSourceTimer, portMAX_DELAY );
    configASSERT( xTimerOk == pdPASS );

    console_print( "07_interrupts: deferred-ISR demo, simulated IRQ every %d ms\n",
                   IRQ_SOURCE_PERIOD_MS );
    console_print( "07_interrupts: starting scheduler\n" );

    vTaskStartScheduler();

    /* 调度器正常情况下不会返回;走到这里说明堆不够建 idle/timer 任务,属致命错误。 */
    for( ; ; )
    {
    }

    return 0;
}
