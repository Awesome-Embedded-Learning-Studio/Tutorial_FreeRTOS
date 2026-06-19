cmake_minimum_required(VERSION 3.15)

project( 09_event-groups )

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

add_executable( 09_event-groups
                console.c
                app_hooks.c
                main.c
              )

target_include_directories( 09_event-groups
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries( 09_event-groups freertos_kernel freertos_config )
