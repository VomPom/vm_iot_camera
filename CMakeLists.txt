cmake_minimum_required(VERSION 3.22.1)
project(vm_iot)

set(CMAKE_CXX_STANDARD 17)


# 严格警告
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-rtsp-1.0
        gstreamer-rtsp-server-1.0)

find_package(yaml-cpp REQUIRED)
find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)

# 递归收集 src/ 下所有 .cpp，排除独立可执行体 cli/。
# 选用 GLOB_RECURSE 是有意为之：源码按职责分多目录（pipeline/filter/branches/control/...），
# 新增子目录无需再回来改 CMake。CMake 不会自动感知 glob 变化——
# 需要新增/删除文件时请手动 cmake --build 时附带 reconfigure，或重跑 cmake。
file(GLOB_RECURSE src-files CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(FILTER src-files EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/src/cli/")

add_executable(vm_iot
        ${src-files})

# 扁平 include 风格：所有头文件直接 #include "xxx.h"，不带子目录前缀。
# 每新增一个 src/ 子目录，请同步加到这里。
target_include_directories(vm_iot
        PUBLIC
        src/app
        src/common
        src/pipeline
        src/filter
        src/branches
        src/branches/snapshot
        src/branches/record
        src/control
        src/rtsp
        src/util
)


target_link_libraries(vm_iot PUBLIC
        PkgConfig::GST yaml-cpp spdlog::spdlog Threads::Threads)

# ---- CLI 工具 iotcamctl ----
# 独立瘦客户端，仅依赖 libc/libstdc++，与 daemon 通过 FIFO 协议通信。
# 故意不放进上面的 src-files glob：CLI 自带 main，与 daemon 互斥。
add_executable(vm_iot_ctl
        ${CMAKE_SOURCE_DIR}/src/cli/iotcamctl.cpp)

# ---- 调试工具 probe_dev ----
# Linux 设备能力手动验证工具：跑 ./probe_dev /dev/video0 与
# `v4l2-ctl --list-formats-ext` 输出对比。仅依赖 v4l2_prober + caps_ranker。
add_executable(probe_dev
        ${CMAKE_SOURCE_DIR}/tools/probe_dev.cpp
        ${CMAKE_SOURCE_DIR}/src/util/v4l2_prober.cpp
        ${CMAKE_SOURCE_DIR}/src/util/caps_ranker.cpp)
target_include_directories(probe_dev PRIVATE
        ${CMAKE_SOURCE_DIR}/src/util
        ${CMAKE_SOURCE_DIR}/src/common)
target_link_libraries(probe_dev PRIVATE spdlog::spdlog)

# ---- 安装规则（make install / cpack 时使用）----
install(TARGETS vm_iot vm_iot_ctl RUNTIME DESTINATION bin)

# ---- 资源同步 ----
add_custom_command(TARGET vm_iot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
        $<TARGET_FILE_DIR:vm_iot>/config
        COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${CMAKE_SOURCE_DIR}/config
        $<TARGET_FILE_DIR:vm_iot>/config
        COMMENT "Linking config/ -> $<TARGET_FILE_DIR:vm_iot>/config (symlink)"
        VERBATIM)

add_custom_command(TARGET vm_iot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
        $<TARGET_FILE_DIR:vm_iot>/shaders
        COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:vm_iot>/shaders
        COMMENT "Linking shaders/ -> $<TARGET_FILE_DIR:vm_iot>/shaders (symlink)"
        VERBATIM)

# ---- 单元测试 ----
# 通过 -DBUILD_TESTING=OFF 可关闭测试构建
option(BUILD_TESTING "Build unit tests" ON)
if (BUILD_TESTING)
    enable_testing()

    # 通过 FetchContent 拉取 GoogleTest，无需系统预装
    include(FetchContent)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
    )
    # 防止 GoogleTest 覆盖父项目的编译选项
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    find_package(GTest REQUIRED)
    include_directories(${GTEST_INCLUDE_DIRS})
    add_subdirectory(tests)
endif ()