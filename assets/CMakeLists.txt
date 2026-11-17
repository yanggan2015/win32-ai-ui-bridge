cmake_minimum_required(VERSION 3.16)
project(ai_controllable_win32 C)

# ON  = 默认：编译桥接层，AI 可通过 HTTP 操控
# OFF = 精简包：不编译桥接层，header 空操作宏生效
option(UI_BRIDGE_ENABLE "编译 AI 可控桥接层（默认开）" ON)
# 兼容旧选项名
if(DEFINED BUILD_TEST_BRIDGE)
  set(UI_BRIDGE_ENABLE ${BUILD_TEST_BRIDGE} CACHE BOOL "" FORCE)
endif()

add_executable(demo_win32 WIN32 demo_win32.c)
target_include_directories(demo_win32 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(UI_BRIDGE_ENABLE)
    target_sources(demo_win32 PRIVATE
        win_ui_bridge.c
        third_party/mongoose.c
        third_party/cJSON.c)
    target_compile_definitions(demo_win32 PRIVATE UI_BRIDGE_ENABLE)
    message(STATUS "桥接层：开启（AI 可控）")
else()
    message(STATUS "桥接层：关闭（精简包，无 HTTP 端口）")
endif()

target_link_libraries(demo_win32 PRIVATE ws2_32 comctl32 user32 gdi32)

if(MSVC)
    target_compile_options(demo_win32 PRIVATE /W4)
else()
    target_compile_options(demo_win32 PRIVATE -Wall -Wextra)
endif()
