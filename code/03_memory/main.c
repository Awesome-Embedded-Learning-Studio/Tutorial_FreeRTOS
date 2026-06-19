/*
 * 03_memory —— 堆内存管理 demo。
 *
 * 目标:亲手操作 pvPortMalloc / vPortFree,配合 xPortGetFreeHeapSize 和
 * xPortGetMinimumEverFreeHeapSize 两把「探针」,直观看到:
 *   - 每次分配让 free heap 掉多少;
 *   - 释放之后 free heap 涨不回来(取决于哪套 heap 实现);
 *   - 一路上 free heap 的「最低水位」是怎么被记录的。
 *
 * 本 demo 在 CMakeLists 里把 FREERTOS_HEAP 选成了 4(heap_4),并把
 * configTOTAL_HEAP_SIZE 抬到 2MB。两件事都说一下为什么:
 *
 * 一是为什么不用 02_environment 那套默认的 heap_3。heap_3 只是宿主机 malloc 的薄包装,
 * 它压根不实现 xPortGetFreeHeapSize / xPortGetMinimumEverFreeHeapSize 这两个探针
 * ——而这俩正是本 demo 的眼睛。heap_4 自己维护一块 ucHeap[] 静态数组,带合并相邻
 * 空闲块(coalesce)的机制,所以 vPortFree 之后 free heap 会精确涨回来,而且全程
 * 追踪历史最低水位 min_ever。这两个数对比起来讲故事特别清楚。
 *
 * 二是为什么堆要 2MB 这么大。POSIX port 下 configMINIMAL_STACK_SIZE 被设成
 * PTHREAD_STACK_MIN(16384),而栈深度的单位是「字」(StackType_t,64 位上是 8 字节),
 * 所以一个任务光栈就要 16384×8 = 128KB——一个 Memory 任务就能从 2MB 的堆里吃掉
 * 13 万多字节。这就是为什么你会看到 baseline 的 free heap 一下掉了 130KB;真 MCU
 * 上一个任务栈通常只有几百到几千字节,完全不会有这种体量,这是 host 模拟独有的现象。
 *
 * 所有 FreeRTOS 必需的回调样板在 app_hooks.c,这里只放 demo 逻辑。
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "console.h"

/* 探针:把当前 free heap 和历史最低 free heap 一起打出来。
 * 两个数能讲清楚两件事——前者是「现在还剩多少」,后者是「峰值时被吃掉多少」。 */
static void prvReportHeap( const char *pcTag )
{
    size_t xFreeNow = xPortGetFreeHeapSize();
    size_t xFreeMin = xPortGetMinimumEverFreeHeapSize();

    console_print( "[heap] %-12s free=%6lu  min_ever=%6lu\n",
                   pcTag,
                   ( unsigned long ) xFreeNow,
                   ( unsigned long ) xFreeMin );
}

/* 一个会从堆里分配的「传感器读数」对象。
 * 故意带个时间戳和原始 buffer,让它读起来像个真实的小结构体。 */
typedef struct SensorReading
{
    int32_t lTimestampMs;
    int32_t lValue;
    uint8_t ucSlot;
} SensorReading_t;

/* 内存任务:分配一批对象、观察 free heap 下落,再逐个释放、观察 free heap 回升。
 * 每个动作之间留一点 delay,让输出可读、也给别的任务运行的机会。 */
static void prvMemoryTask( void *pvParameters )
{
    ( void ) pvParameters;

    /* 分配多少个对象、每个多大,集中在这两行,方便调参复现。
     * 数量小于 configTOTAL_HEAP_SIZE 能容纳的上限,确保不会触发 malloc failed hook。 */
    const size_t uxObjectCount = 8;
    const size_t uxPerObject = sizeof( SensorReading_t );
    SensorReading_t *pxReadings[ 8 ] = { NULL };

    console_print( "memory task: object size = %lu bytes, will allocate %lu of them\n",
                   ( unsigned long ) uxPerObject,
                   ( unsigned long ) uxObjectCount );

    /* 起点:scheduler 已经把 idle / timer 两个任务的栈和 TCB 从堆里扣掉了,
     * 所以 free heap 一定小于 configTOTAL_HEAP_SIZE,这里先记一个基准。 */
    vTaskDelay( pdMS_TO_TICKS( 200 ) );
    prvReportHeap( "baseline" );

    /* 第一阶段:逐个分配。每分配一个就探一次针,你会看到 free heap 一格格往下掉,
     * 而且每掉的量比 sizeof(SensorReading_t) 多——那多出来的部分是 heap_3 底层
     * malloc 的对齐与元数据开销,不是 FreeRTOS 收的,是 glibc 收的。 */
    for( size_t i = 0; i < uxObjectCount; i++ )
    {
        pxReadings[ i ] = ( SensorReading_t * ) pvPortMalloc( uxPerObject );

        if( pxReadings[ i ] != NULL )
        {
            pxReadings[ i ]->lTimestampMs = ( int32_t ) ( i * 100 );
            pxReadings[ i ]->lValue = ( int32_t ) ( i * i );
            pxReadings[ i ]->ucSlot = ( uint8_t ) i;
        }

        console_print( "memory task: alloc[%lu] => %s\n",
                       ( unsigned long ) i,
                       pxReadings[ i ] != NULL ? "ok" : "FAILED" );
        prvReportHeap( "after alloc" );

        vTaskDelay( pdMS_TO_TICKS( 150 ) );
    }

    /* 第二阶段:逐个释放。heap_3 把 free 转给 glibc 的 free,glibc 把块还给它的池子,
     * 于是 free heap 一格格涨回来。注意它不一定恰好回到 baseline——glibc 有自己的
     * 碎片/阈值策略,不保证每 free 一块 free heap 就精确回升同样的字节数。 */
    for( size_t i = 0; i < uxObjectCount; i++ )
    {
        if( pxReadings[ i ] != NULL )
        {
            /* 释放前先清掉内容,养成「释放即不再访问」的习惯,避免野指针。 */
            memset( pxReadings[ i ], 0, uxPerObject );
            vPortFree( pxReadings[ i ] );
            pxReadings[ i ] = NULL;
        }

        console_print( "memory task: free[%lu]\n", ( unsigned long ) i );
        prvReportHeap( "after free " );

        vTaskDelay( pdMS_TO_TICKS( 150 ) );
    }

    console_print( "memory task: cycle done, looping\n" );

    /* 循环再来一遍,观察 free heap 是否稳定(分配-释放应该是「守恒」的,
     * 不会越跑越漏)。 */
    for( ; ; )
    {
        for( size_t i = 0; i < uxObjectCount; i++ )
        {
            pxReadings[ i ] = ( SensorReading_t * ) pvPortMalloc( uxPerObject );
            if( pxReadings[ i ] != NULL )
            {
                vPortFree( pxReadings[ i ] );
                pxReadings[ i ] = NULL;
            }
        }
        prvReportHeap( "steady cycle" );
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }
}

int main( void )
{
    /* 关掉 stdout 全缓冲,避免重定向/管道时输出丢失(见 02_environment 排坑章)。 */
    setvbuf( stdout, NULL, _IOLBF, 0 );

    console_init();

    /* heap_4 的堆区是静态数组 ucHeap[configTOTAL_HEAP_SIZE],在编译期就存在,
     * 这就是「内存在任务之前」的直接证据:还没进 scheduler、一个任务没创建,
     * 我们就已经能说出堆一共多大。注意 heap_4 是惰性初始化的——堆还没被
     * 第一次 pvPortMalloc 碰过时,xPortGetFreeHeapSize() 会返回 0(不是满),
     * 所以真正的「baseline」我们在任务里、scheduler 把 idle/timer 栈扣掉之后再探。 */
    console_print( "03_memory: total heap = %lu bytes\n",
                   ( unsigned long ) configTOTAL_HEAP_SIZE );

    xTaskCreate( prvMemoryTask, "Memory", configMINIMAL_STACK_SIZE, NULL,
                 tskIDLE_PRIORITY + 1, NULL );

    console_print( "03_memory: starting scheduler\n" );

    vTaskStartScheduler();

    for( ; ; )
    {
    }

    return 0;
}
