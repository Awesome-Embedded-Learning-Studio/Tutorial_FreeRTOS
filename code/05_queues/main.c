/*
 * 05_queues —— 队列管理独立 demo。
 *
 * 要把队列这一个原语讲透,我们需要让两件事肉眼可见:
 *   1. 「阻塞读」:接收任务在空队列上 xQueueReceive 时,会被挂进 blocked,
 *      不占 CPU,直到有数据进来才被唤醒——这是 blocked 状态除了 vTaskDelay 之外的
 *      另一个来源,也是队列最值钱的能力。
 *   2. 「超时返回」:xQueueReceive 的第三个参数是「最多等多久」。给一个有限超时,
 *      它等不到数据就会超时返回 pdFALSE,任务能据此知道「这段时间没人投递」,
 *      而不是被永久卡死。
 *
 * 为了一起演示这两点,demo 安排了一个数据生产者和一个消费者:
 *   - Sender(优先级较高):周期性地往队列里投一帧带时间戳的「采样」。它会
 *     切换两种节奏——先连续快发(Receiver 每次都能被唤醒、立刻消费),然后
 *     故意停发一段比 Receiver 超时还长的空窗(队列被抽空、没人投递,
 *     Receiver 就在空队列上等满超时、超时返回),循环往复。
 *   - Receiver(优先级更低):阻塞读队列,读到的就显示;读不到(超时)就报一行
 *     「等了 X ms 没数据」。
 *
 * Receiver 优先级压低是有意的:它要么「被队列唤醒」(有数据)、要么「超时醒来」
 * (空窗),平时根本不抢 CPU——正好把「blocked 不占 CPU」这件事和
 * 「优先级高的 Sender 一投递就把它叫醒」这件事一起演清楚。这样调度轨迹里
 * 你能同时看到:有数据时 Receiver 被唤醒消费、空窗时 Receiver 超时返回。
 *
 * 还有一个本章最该带走的语义:消息是「按值拷贝」的。队列 API 不传指针,
 * 你喂给 xQueueSend 的是一块数据的拷贝,内核会把它 memcpy 进队列内部缓冲区,
 * 取的时候再 memcpy 出来。所以这里 Sender 投完一个局部结构体、函数返回,
 * 那块栈上的内存随之失效也无所谓——Receiver 拿到的是副本,和发送方彻底解耦。
 *
 * 所有 FreeRTOS 必需的回调样板(vAssertCalled / 各类 hook / 静态分配内存)
 * 都在 app_hooks.c 里,这个文件只放 demo 本身的逻辑。CMakeLists 已把
 * FREERTOS_HEAP 选成 4、configTOTAL_HEAP_SIZE 抬到 2MB,原因和 03_memory 一样:
 * POSIX port 的任务栈一个个都有 128KB,固定大小的堆必须给足。
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "console.h"

/* 任务优先级。configMAX_PRIORITIES=7,合法 0..6。
 * Sender 给 +3,Receiver 给 +2(低一档),这样「投递优先、消费随后」,
 * Receiver 平时 blocked,只有队列里进了数据才会被唤醒跑。 */
#define prioSENDER      ( tskIDLE_PRIORITY + 3 )
#define prioRECEIVER    ( tskIDLE_PRIORITY + 2 )

/* 队列容量:故意只放 4 帧。容量小一点,空窗和「有数据」的差异在输出里更醒目。 */
#define QUEUE_LENGTH    ( 4 )

/* Receiver 每次最多阻塞等多久(1500ms)。这个值是精心挑的:
 * Sender「快发」阶段每 300ms 来一帧,Receiver 总能在超时前被唤醒;
 * 而当 Sender 进入「停发空窗」(停 3000ms,比超时长)时,Receiver 就会真的
 * 等满 1500ms 超时,把「超时返回」这一幕演出来。 */
#define RECEIVE_TIMEOUT_MS   ( 1500 )

/* Sender 的两个节奏。BURST:每 BURST_PERIOD_MS 投一帧,投 BURST_FRAMES 帧;
 * SILENCE:停发 SILENCE_MS 毫秒,这段时间制造 Receiver 的超时。 */
#define BURST_PERIOD_MS      ( 300 )
#define BURST_FRAMES         ( 6 )
#define SILENCE_MS           ( 3000 )

/* 投递进队列的一帧数据。就是个带序号和 tick 时间戳的小结构体——
 * 关键点:我们靠队列的「按值拷贝」语义投递它,Sender 投完就能函数返回,
 * 不用担心 Receiver 什么时候来取、取的时候那块内存还在不在。 */
typedef struct
{
    uint32_t ulSeq;     /* 帧序号,单调递增,方便在输出里追踪 */
    uint32_t ulTick;    /* 投递时刻的 tick 数(xTaskGetTickCount),相当时间戳 */
} SampleFrame_t;

/* 队列句柄。在进 scheduler 之前 xQueueCreate 创建好,任务里直接用。 */
static QueueHandle_t xSampleQueue = NULL;

/* ---- 工具:把当前 tick 当「从启动起多少毫秒」前缀打印出来 ---- */
static void prvPrintTickPrefix( void )
{
    /* tick 频率是 1000Hz(configTICK_RATE_HZ),所以 1 tick = 1ms,直接当毫秒看。
     * 真硬件上 tick 频率常是 100Hz 或更高,换算系数会变,这里 host 模拟下恰好是 1:1。 */
    console_print( "[%5lu ms] ",
                   ( unsigned long ) xTaskGetTickCount() );
}

/* ---- Sender:先快发 BURST_FRAMES 帧,再停发 SILENCE_MS 毫秒,循环 ---- */
static void prvSenderTask( void *pvParameters )
{
    ( void ) pvParameters;
    SampleFrame_t xFrame;
    uint32_t ulSeq = 0;

    for( ; ; )
    {
        /* 快发阶段:连续投 BURST_FRAMES 帧,每帧间隔 BURST_PERIOD_MS。
         * 这段时间 Receiver 每次都能在超时前被队列唤醒、拿到数据。 */
        for( uint32_t i = 0; i < BURST_FRAMES; i++ )
        {
            xFrame.ulSeq = ulSeq;
            xFrame.ulTick = xTaskGetTickCount();

            /* xQueueSend 的第二个参数是「待拷贝数据的地址」,不是数据本身——
             * 内核会从这块地址 memcpy sizeof(SampleFrame_t) 字节进队列。
             * 第三个参数是「队列满了等多久」:这里给 0(portMAX_DELAY 才是无限等),
             * 表示「满了就立刻放弃」(返回 errQUEUE_FULL),不阻塞。demo 队列
             * 容量 4、消费速度跟得上,正常不会满;真满了我们也会报告,不偷偷丢。 */
            BaseType_t xOk = xQueueSend( xSampleQueue, &xFrame, 0 );

            prvPrintTickPrefix();
            if( xOk == pdPASS )
            {
                console_print( "sender   send  seq=%lu\n",
                               ( unsigned long ) ulSeq );
            }
            else
            {
                /* 队列满:消费端没跟上才会发生,报出来比静默丢帧强。 */
                console_print( "sender   send  seq=%lu  QUEUE FULL (dropped)\n",
                               ( unsigned long ) ulSeq );
            }

            ulSeq++;
            vTaskDelay( pdMS_TO_TICKS( BURST_PERIOD_MS ) );
        }

        /* 停发阶段:停 SILENCE_MS 毫秒。这段时间队列会被 Receiver 抽空、没人投递,
         * Receiver 就会在空队列上等满超时——超时返回这一幕就发生在这个空窗里。 */
        prvPrintTickPrefix();
        console_print( "sender   goes SILENT for %d ms (expect receiver timeouts)\n",
                       SILENCE_MS );
        vTaskDelay( pdMS_TO_TICKS( SILENCE_MS ) );
    }
}

/* ---- Receiver:阻塞读队列,读不到就超时返回 ---- */
static void prvReceiverTask( void *pvParameters )
{
    ( void ) pvParameters;
    SampleFrame_t xFrame;

    for( ; ; )
    {
        /* 核心:第三个参数给有限超时。队列空就 blocked 等,最多等 RECEIVE_TIMEOUT_MS;
         * 期间有数据进来就被唤醒、拿到数据返回 pdPASS;
         * 若一直没数据,等满超时返回 errQUEUE_EMPTY(就是 pdFALSE)。 */
        BaseType_t xGot = xQueueReceive( xSampleQueue, &xFrame,
                                         pdMS_TO_TICKS( RECEIVE_TIMEOUT_MS ) );

        prvPrintTickPrefix();
        if( xGot == pdPASS )
        {
            /* 拿到的 xFrame 是队列里那份的「副本」,和发送方再无瓜葛。
             * ulTick 是投递时刻,和「现在」的差值,就是这一帧在队列里排了多久。 */
            console_print( "receiver got seq=%lu (queued %lu ms ago)\n",
                           ( unsigned long ) xFrame.ulSeq,
                           ( unsigned long ) ( xTaskGetTickCount() - xFrame.ulTick ) );
        }
        else
        {
            /* 这一行就是「超时返回」的铁证:队列空了 RECEIVE_TIMEOUT_MS 还没数据。 */
            console_print( "receiver TIMEOUT: queue empty for %d ms\n",
                           RECEIVE_TIMEOUT_MS );
        }
    }
}

int main( void )
{
    /* 关掉 stdout 全缓冲,避免重定向/管道时输出丢失(见 02_environment 排坑章)。 */
    setvbuf( stdout, NULL, _IOLBF, 0 );

    console_init();

    /* 队列必须在进 scheduler 之前创建好。一次 xQueueCreate 开出来:
     * 一个内部环形缓冲区,每个槽位 sizeof(SampleFrame_t) 字节,共 QUEUE_LENGTH 个槽。
     * 返回 NULL 表示创建失败(heap 不够)——这种致命错误直接断言。 */
    xSampleQueue = xQueueCreate( QUEUE_LENGTH, sizeof( SampleFrame_t ) );
    configASSERT( xSampleQueue != NULL );

    xTaskCreate( prvSenderTask, "Sender", configMINIMAL_STACK_SIZE, NULL,
                 prioSENDER, NULL );
    xTaskCreate( prvReceiverTask, "Receiver", configMINIMAL_STACK_SIZE, NULL,
                 prioRECEIVER, NULL );

    console_print( "05_queues: queue length=%d, frame size=%lu bytes, receive timeout=%d ms\n",
                   QUEUE_LENGTH,
                   ( unsigned long ) sizeof( SampleFrame_t ),
                   RECEIVE_TIMEOUT_MS );
    console_print( "05_queues: starting scheduler\n" );

    vTaskStartScheduler();

    for( ; ; )
    {
    }

    return 0;
}
