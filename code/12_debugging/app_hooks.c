/*
 * app_hooks.c —— FreeRTOS 应用层必需的回调样板代码。
 *
 * 这些函数由内核在特定时机调用,或因 FreeRTOSConfig.h 的配置开关而必需。
 * 每个 demo 都得提供它们,但内容基本固定,所以单独抽出来:main.c 只管 demo 逻辑,
 * 这一份样板各 demo 原样复制即可,不需要改动。
 *
 * 对应 FreeRTOSConfig.h 里的:
 *   configUSE_MALLOC_FAILED_HOOK / configUSE_IDLE_HOOK / configUSE_TICK_HOOK
 *   configUSE_DAEMON_TASK_STARTUP_HOOK / configSUPPORT_STATIC_ALLOCATION / configUSE_TIMERS
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"

/* configASSERT(见 FreeRTOSConfig.h)失败时回调到这里。打印一行后停下,
 * 方便定位;调试器里把 ulSetToNonZeroInDebuggerToContinue 改成非 0 可继续。 */
void vAssertCalled( const char * const pcFileName,
                    unsigned long ulLine )
{
    volatile uint32_t ulSetToNonZeroInDebuggerToContinue = 0;

    ( void ) pcFileName;
    ( void ) ulLine;

    printf( "ASSERT FAILED: %s:%lu\n", pcFileName, ulLine );
    fflush( stdout );

    taskENTER_CRITICAL();
    {
        while( ulSetToNonZeroInDebuggerToContinue == 0 )
        {
        }
    }
    taskEXIT_CRITICAL();
}

/* configUSE_MALLOC_FAILED_HOOK=1:pvPortMalloc 分配失败时调用。 */
void vApplicationMallocFailedHook( void )
{
    vAssertCalled( __FILE__, __LINE__ );
}

/* configUSE_IDLE_HOOK=1:空闲任务每次循环调用。
 * 这里 usleep 主动让出 CPU——POSIX port 下空闲任务不做让出会把一个核占满。 */
void vApplicationIdleHook( void )
{
    usleep( 15000 );
}

/* configUSE_TICK_HOOK=1:每个 tick 中断调用一次。demo 暂不使用,留空。 */
void vApplicationTickHook( void )
{
}

/* configCHECK_FOR_STACK_OVERFLOW:POSIX port 下栈溢出检测实际不生效(见 pitfalls 章),
 * 这里仍提供实现以备真硬件使用;一旦触发就停下。 */
void vApplicationStackOverflowHook( TaskHandle_t pxTask,
                                    char * pcTaskName )
{
    ( void ) pcTaskName;
    ( void ) pxTask;
    vAssertCalled( __FILE__, __LINE__ );
}

/* configUSE_DAEMON_TASK_STARTUP_HOOK=1:定时器服务任务首次运行时调用一次。 */
void vApplicationDaemonTaskStartupHook( void )
{
}

/* configSUPPORT_STATIC_ALLOCATION=1:内核要 app 提供空闲任务的静态内存。 */
void vApplicationGetIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
                                    StackType_t ** ppxIdleTaskStackBuffer,
                                    configSTACK_DEPTH_TYPE * pulIdleTaskStackSize )
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t uxIdleTaskStack[ configMINIMAL_STACK_SIZE ];

    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/* configSUPPORT_STATIC_ALLOCATION=1 且 configUSE_TIMERS=1:内核要 app 提供定时器任务的静态内存。 */
void vApplicationGetTimerTaskMemory( StaticTask_t ** ppxTimerTaskTCBBuffer,
                                     StackType_t ** ppxTimerTaskStackBuffer,
                                     configSTACK_DEPTH_TYPE * pulTimerTaskStackSize )
{
    static StaticTask_t xTimerTaskTCB;
    static StackType_t uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];

    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}
