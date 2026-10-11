cmake_minimum_required(VERSION 3.15)
project(XConnect C)

set(CMAKE_C_STANDARD 99)

# X-Plane 插件必须无动态 MSVC 运行库依赖（否则加载 .xpl 前需解依赖，
# 目标机可能缺少 VCRUNTIME140.dll / UCRT）。使用静态 CRT（/MT）。
# 需 CMake >= 3.15（CMP0091 新行为）方能生效。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# X-Plane SDK 路径。默认使用仓库内置 SDK（XConnect/SDK，XPSDK 43.0）。
# 也可覆盖：cmake -DXPlaneSDK=<路径>
# 兼容三种布局：
#   仓库内置:    <SDK>/CHeaders/XPLM/XPLMDefs.h + <SDK>/Libraries/Win/XPLM_64.lib
#   官方 SDK 包: <SDK>/SDK/CHeaders/XPLM/...  + <SDK>/SDK/Libraries/Win/XPLM.lib
#   头文件仓库:  <SDK>/XPLM/XPLMDefs.h（github.com/x-plane/xplm，仅头文件）
set(XPlaneSDK "${CMAKE_CURRENT_SOURCE_DIR}/SDK" CACHE PATH
    "X-Plane SDK directory")
file(TO_CMAKE_PATH "${XPlaneSDK}" XPlaneSDK)

if(EXISTS "${XPlaneSDK}/CHeaders/XPLM")
    # 仓库内置 SDK 布局
    set(XPLM_BASE "${XPlaneSDK}/CHeaders/XPLM")
    set(XPLM_LIB_HINTS "${XPlaneSDK}/Libraries/Win")
elseif(EXISTS "${XPlaneSDK}/SDK/CHeaders/XPLM")
    set(XPLM_BASE "${XPlaneSDK}/SDK/CHeaders/XPLM")
    set(XPLM_LIB_HINTS "${XPlaneSDK}/SDK/Libraries/Win")
elseif(EXISTS "${XPlaneSDK}/XPLM")
    # 头文件仓库（x-plane/xplm），无导入库
    set(XPLM_BASE "${XPlaneSDK}/XPLM")
    set(XPLM_LIB_HINTS "")
else()
    message(FATAL_ERROR "未找到 X-Plane SDK（应在 ${XPlaneSDK}，或 cmake -DXPlaneSDK=<路径> 覆盖）")
endif()

add_library(XConnect MODULE
    src/xconnect.c src/datarefs.c src/players.c src/labels.c src/json.c
    src/net.c src/csl.c)
target_include_directories(XConnect PRIVATE "${XPLM_BASE}" "${CMAKE_CURRENT_SOURCE_DIR}/src")

# SDK 430 起不再自动定义 XPLM200 系列版本宏，须由插件显式声明，
# 否则 XPLMObjectRef / XPLMDrawInfo_t 等将被 #if defined(XPLMxxx) 隐藏。
target_compile_definitions(XConnect PRIVATE
    XPLM200 XPLM210 XPLM300 XPLM400 XPLM410 XPLM420 XPLM430)

# 插件必须无动态链接依赖（X-Plane 加载 .xpl 前需解依赖）
set_target_properties(XConnect PROPERTIES
    PREFIX ""
    SUFFIX ".xpl")

if(WIN32)
    target_compile_definitions(XConnect PRIVATE _CRT_SECURE_NO_WARNINGS IBM)
    # /utf-8：源码为 UTF-8。否则在中文 Windows（代码页 936）下 MSVC 按 GBK
    # 解析，中文注释的尾字节会被当作前导字节吞掉换行，导致行号错乱/编译失败
    # （en-US 的 CI 用 cp1252 不触发，故此前只在本地中文系统复现）。
    target_compile_options(XConnect PRIVATE /O2 /W3 /utf-8)
    # 导入库：SDK 430 为 XPLM_64.lib；旧版为 XPLM.lib
    find_library(XPLM_LIB NAMES XPLM_64 XPLM
        HINTS "${XPLM_LIB_HINTS}" "${XPlaneSDK}/lib")
    if(XPLM_LIB)
        target_link_libraries(XConnect PRIVATE "${XPLM_LIB}" ws2_32)
    else()
        message(FATAL_ERROR "未找到 XPLM_64.lib / XPLM.lib（Windows 需官方 SDK 包）")
    endif()
elseif(APPLE)
    target_compile_definitions(XConnect PRIVATE APL)
    set_target_properties(XConnect PROPERTIES
        BUNDLE TRUE PREFIX "" SUFFIX ".xpl")
else()
    target_compile_definitions(XConnect PRIVATE LIN)
    target_compile_options(XConnect PRIVATE -O2 -Wall -fPIC -shared)
    target_link_libraries(XConnect PRIVATE pthread)
endif()

# 输出到 build 顶层，文件名 XConnect.xpl
# 多配置生成器（Visual Studio）默认会追加 /Release 等子目录，
# 这里为所有配置统一输出目录，保证产物固定在 build/XConnect.xpl
foreach(_cfg RELEASE DEBUG RELWITHDEBINFO MINSIZEREL)
    set_target_properties(XConnect PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}")
endforeach()
set_target_properties(XConnect PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")