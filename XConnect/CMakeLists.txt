cmake_minimum_required(VERSION 3.13)
project(XConnect C)

set(CMAKE_C_STANDARD 99)

# X-Plane SDK 路径：-DXPlaneSDK=<路径>
# 兼容两种布局：
#   官方 SDK 包:  <SDK>/SDK/CHeaders/XPLM/XPLM.h + <SDK>/SDK/Libraries/Win/XPLM.lib
#   头文件仓库:  <SDK>/XPLM/XPLM.h（github.com/x-plane/xplm，仅头文件）
set(XPlaneSDK "" CACHE PATH "X-Plane SDK directory")
if(NOT XPlaneSDK)
    message(FATAL_ERROR "未指定 X-Plane SDK 路径：cmake -DXPlaneSDK=<SDK路径>")
endif()
file(TO_CMAKE_PATH "${XPlaneSDK}" XPlaneSDK)

find_path(XPLM_INC XPLM.h
    HINTS "${XPlaneSDK}/XPLM"
          "${XPlaneSDK}/SDK/CHeaders/XPLM"
          "${XPlaneSDK}/CHeaders/XPLM")
if(NOT XPLM_INC)
    message(FATAL_ERROR "未找到 XPLM.h（检查 SDK 布局：XPLM/ 或 SDK/CHeaders/XPLM/）")
endif()

add_library(XConnect MODULE src/xconnect.c src/json.c src/net.c src/csl.c)
target_include_directories(XConnect PRIVATE "${XPLM_INC}" "${CMAKE_CURRENT_SOURCE_DIR}/src")

# 插件必须无动态链接依赖（X-Plane 加载 .xpl 前需解依赖）
set_target_properties(XConnect PROPERTIES
    PREFIX ""
    SUFFIX ".xpl")

if(WIN32)
    target_compile_definitions(XConnect PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(XConnect PRIVATE /O2 /W3)
    # XPLM.lib 导入库（官方 SDK 包提供）
    find_library(XPLM_LIB XPLM
        HINTS "${XPlaneSDK}/lib" "${XPlaneSDK}/SDK/Libraries/Win")
    if(XPLM_LIB)
        target_link_libraries(XConnect PRIVATE "${XPLM_LIB}" ws2_32)
    else()
        message(FATAL_ERROR "未找到 XPLM.lib（Windows 需官方 SDK 包，见 README）")
    endif()
elseif(APPLE)
    set_target_properties(XConnect PROPERTIES
        BUNDLE TRUE PREFIX "" SUFFIX ".xpl")
else()
    target_compile_options(XConnect PRIVATE -O2 -Wall -fPIC -shared)
    target_link_libraries(XConnect PRIVATE pthread)
endif()

# 输出到 build 顶层，文件名 XConnect.xpl
set_target_properties(XConnect PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")