cmake_minimum_required(VERSION 3.15)

project( 08_resources )

add_compile_options( -D_WINDOWS_ )

add_compile_options( -Wall -Wextra -Wpedantic )

if( NO_TRACING )
    set( NO_TRACING 1 )
    add_compile_options( -DprojENABLE_TRACING=0 )
else()
    set( NO_TRACING 0 )
    add_compile_options( -DprojENABLE_TRACING=1 )
endif()

# FreeRTOSConfig.h 强制要求这两个宏在命令行定义,照 00-bootstrap 配方补齐
add_compile_options( -DprojCOVERAGE_TEST=0 )
add_compile_options( -DTRACE_ON_ENTER=0 )

set( CMAKE_BUILD_TYPE "debug" )

set(CMAKE_C_FLAGS_DEBUG "-O0 -g3")

set( FREERTOS_KERNEL_PATH "../../third_party/FreeRTOS/FreeRTOS/Source" )
set( FREERTOS_PLUS_TRACE_PATH "../../third_party/FreeRTOS/FreeRTOS-Plus/Source/FreeRTOS-Trace" )

# FreeRTOS 配置接口
add_library( freertos_config INTERFACE )

target_include_directories( freertos_config
    INTERFACE
        ./
)

# 内存管理方案:heap_3(对宿主机 malloc 的线程安全包装)
set( FREERTOS_HEAP "4" CACHE STRING "" FORCE)

# 平台 port:POSIX
set( FREERTOS_PORT "GCC_POSIX" CACHE STRING "" FORCE)

# 引入 FreeRTOS 内核
add_subdirectory( ${FREERTOS_KERNEL_PATH} ${CMAKE_CURRENT_BINARY_DIR}/FreeRTOS-Kernel )

add_executable( 08_resources
                console.c
                app_hooks.c
                main.c
              )

target_include_directories( 08_resources
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# 锁类型开关:OFF(默认)用二值信号量(无优先级继承,复现优先级反转);
# ON 用互斥量(带优先级继承,破解反转)。命令行 -DUSE_MUTEX=1 切换。
option( USE_MUTEX "Use a mutex (priority inheritance) instead of a binary semaphore" OFF )
if( USE_MUTEX )
    target_compile_definitions( 08_resources PRIVATE USE_MUTEX=1 )
endif()

target_link_libraries( 08_resources freertos_kernel freertos_config )
