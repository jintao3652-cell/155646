cmake_minimum_required(VERSION 3.15)
project(MConnect C)

# MConnect 依赖 SimConnect（仅 Windows 实现），只在 Windows 构建。
if(NOT WIN32)
    message(FATAL_ERROR "MConnect only supports Windows (MSFS 2020/2024)")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 静态 CRT（/MT）：产物不依赖 VCRUNTIME/MSVCP/UCRT，单个 exe 拷到目标机即可运行。
# 需 CMake >= 3.15（CMP0091 新行为）方能生效。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 源码为 UTF-8（含中文注释），显式 /utf-8，避免非英文代码页下被误解析。
if(MSVC)
    add_compile_options(/utf-8 /W3)
endif()
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

set(MCONNECT_SOURCES
    src/mconnect.c
    src/sc.c
    src/datadefs.c
    src/catalog.c
    src/players.c
    src/control.c
    src/aircraftcaps.c
    src/aircraftdb.c
    src/pmdgbridge.c
    src/wasmbridge.c
    src/inputev.c
    src/ini.c
    src/json.c
    src/net.c
)

# 多配置生成器（Visual Studio）默认输出到 build/<Config>；这里统一到 build 顶层，
# CI 与用户都从 build/MConnect.exe、build/<test>.exe 取产物。
function(mconnect_output_to_build_root target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    foreach(_cfg RELEASE DEBUG RELWITHDEBINFO MINSIZEREL)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}")
    endforeach()
endfunction()

# ---------------- 主程序 ----------------

# 默认产物 MConnect.exe 用 GUI 子系统（WIN32 关键字 → /SUBSYSTEM:WINDOWS），
# 双击/随客户端启动时不再弹黑色控制台窗口。日志照常写入 exe 同目录的
# mconnect.log（xc_logf/xc_infof 都落盘），因此无窗口不影响排查。
# 另编一个 MConnectConsole.exe（控制台子系统），供 --help / --dump-* /
# --scan-only 等需要直接看 stdout 的诊断场景使用，源码完全相同。
add_executable(MConnect WIN32 ${MCONNECT_SOURCES})
target_include_directories(MConnect PRIVATE src)
target_link_libraries(MConnect PRIVATE ws2_32 advapi32)
target_compile_definitions(MConnect PRIVATE MCONNECT_GUI)
mconnect_output_to_build_root(MConnect)

add_executable(MConnectConsole ${MCONNECT_SOURCES})
target_include_directories(MConnectConsole PRIVATE src)
target_link_libraries(MConnectConsole PRIVATE ws2_32 advapi32)
mconnect_output_to_build_root(MConnectConsole)

# ---------------- 单元测试（不需要模拟器 / SimConnect.dll）----------------
# 每个测试自带必要的桩（g_sc、xc_logf 等），只链接被测模块。
# 源文件集合已在 VS2026 下逐个编译运行验证。

enable_testing()

function(mconnect_add_test name)
    add_executable(${name} test/${name}.c ${ARGN})
    target_include_directories(${name} PRIVATE src)
    target_link_libraries(${name} PRIVATE advapi32)
    mconnect_output_to_build_root(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

mconnect_add_test(aircraftdb_test    src/aircraftdb.c src/json.c)
mconnect_add_test(pmdgbridge_test    src/pmdgbridge.c)
mconnect_add_test(inputev_test       src/inputev.c)
mconnect_add_test(wasmbridge_test    src/wasmbridge.c)
mconnect_add_test(catalog_test       src/catalog.c src/ini.c)
mconnect_add_test(control_test
    src/control.c src/inputev.c src/aircraftdb.c src/wasmbridge.c
    src/pmdgbridge.c src/json.c)
mconnect_add_test(radio_test
    src/datadefs.c src/aircraftdb.c src/pmdgbridge.c src/wasmbridge.c
    src/json.c)
mconnect_add_test(aircraftcaps_test
    src/aircraftcaps.c src/inputev.c src/wasmbridge.c)
