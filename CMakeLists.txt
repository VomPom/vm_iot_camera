cmake_minimum_required(VERSION 3.22.1)
project(vm_iot)

set(CMAKE_CXX_STANDARD 17)


# 严格警告
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-base-1.0
        gstreamer-video-1.0
        gstreamer-audio-1.0
        gstreamer-rtsp-1.0
        gstreamer-rtsp-server-1.0)

# ---- 音频通道（capture/encode/RTP）开关 ----
# 默认 ON：编译进可执行文件；OFF 时 audio_branch / alsa_prober / pipeline_builder
# 内的音频段都被 #if VM_IOT_ENABLE_AUDIO=0 短路，二进制运行行为与改造前一致。
# 运行期还有 cfg.audio.enabled 第二级开关，二者任一关闭即不接入音频。
option(VM_IOT_ENABLE_AUDIO "Enable audio capture/encode/RTP path" ON)
message(STATUS "VM_IOT_ENABLE_AUDIO=${VM_IOT_ENABLE_AUDIO}")

# alsa-lib 仅 alsa_prober.cpp 需要：用 snd_pcm_open(O_NONBLOCK) 验证设备可用。
# 缺包时退化为读 /proc/asound/cards 的弱检查，不影响构建。
pkg_check_modules(ALSA IMPORTED_TARGET alsa)
if (ALSA_FOUND)
    message(STATUS "alsa-lib: ${ALSA_VERSION}")
else ()
    message(STATUS "alsa-lib not found, alsa_prober falls back to /proc/asound check")
endif ()

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
        src/branches/pag
        src/branches/audio
        src/control
        src/rtsp
        src/util
        src/plugins/pagfilter
)


target_link_libraries(vm_iot PUBLIC
        PkgConfig::GST yaml-cpp spdlog::spdlog Threads::Threads)

# 音频开关透传到代码层。
if (VM_IOT_ENABLE_AUDIO)
    target_compile_definitions(vm_iot PRIVATE VM_IOT_ENABLE_AUDIO=1)
    if (ALSA_FOUND)
        target_compile_definitions(vm_iot PRIVATE VM_IOT_HAVE_ALSA_LIB=1)
        target_link_libraries(vm_iot PUBLIC PkgConfig::ALSA)
    else ()
        target_compile_definitions(vm_iot PRIVATE VM_IOT_HAVE_ALSA_LIB=0)
    endif ()
else ()
    target_compile_definitions(vm_iot PRIVATE VM_IOT_ENABLE_AUDIO=0 VM_IOT_HAVE_ALSA_LIB=0)
endif ()

# ---- libpag ----
# 目标平台只考虑树莓派 aarch64 Debian/Raspbian。其他平台请自行扩展。
# 运行依赖：sudo apt install libegl-dev libgles2-mesa-dev
set(VM_IOT_ENABLE_LIBPAG ON)
message(STATUS "VM_IOT_ENABLE_LIBPAG=${VM_IOT_ENABLE_LIBPAG}")

# libpag 预编译产物按 "<arch>-<libc>-<distro><ver>" 分目录存放在 third_party/libpag/ 下。
# 当前可用子目录：
#   - aarch64-linux-gnu-ubuntu22  ← UTM 上的 ARM Ubuntu 22.04（当前默认）
#   - aarch64-linux-gnu-ubuntu24  ← ARM Ubuntu 24.04
# 切换环境时改这一行即可（或在 cmake 命令行用 -DVM_IOT_LIBPAG_TRIPLET=... 覆盖）。
set(VM_IOT_LIBPAG_TRIPLET "aarch64-linux-gnu-ubuntu22"
        CACHE STRING "libpag prebuilt subdir under third_party/libpag/")
message(STATUS "VM_IOT_LIBPAG_TRIPLET=${VM_IOT_LIBPAG_TRIPLET}")


if (VM_IOT_ENABLE_LIBPAG)
    set(_libpag_lib     "${CMAKE_SOURCE_DIR}/third_party/libpag/${VM_IOT_LIBPAG_TRIPLET}/lib/libpag.a")
    set(_libpag_include "${CMAKE_SOURCE_DIR}/third_party/libpag/include")

    if (NOT EXISTS "${_libpag_lib}" OR NOT EXISTS "${_libpag_include}/pag/pag.h")
        message(FATAL_ERROR
                "libpag artifacts missing. Expected:\n"
                "  ${_libpag_lib}\n"
                "  ${_libpag_include}/pag/pag.h\n"
                "Check VM_IOT_LIBPAG_TRIPLET (currently '${VM_IOT_LIBPAG_TRIPLET}') "
                "or see docs/task/libpag.md for how to build them.")
    endif ()

    # 系统 Mesa 提供 EGL / GLESv2；libpag.a 不包含这两类符号。
    find_library(VM_IOT_LIBEGL    NAMES EGL    REQUIRED)
    find_library(VM_IOT_LIBGLESV2 NAMES GLESv2 REQUIRED)

    target_compile_definitions(vm_iot PRIVATE VM_IOT_ENABLE_LIBPAG=1)
    target_include_directories(vm_iot PRIVATE "${_libpag_include}")
    target_link_libraries(vm_iot PRIVATE
            "${_libpag_lib}"
            ${VM_IOT_LIBEGL} ${VM_IOT_LIBGLESV2}
            ${CMAKE_DL_LIBS})

    message(STATUS "vm_iot: libpag    -> ${_libpag_lib}")
    message(STATUS "vm_iot: EGL       -> ${VM_IOT_LIBEGL}")
    message(STATUS "vm_iot: GLESv2    -> ${VM_IOT_LIBGLESV2}")
else ()
    target_compile_definitions(vm_iot PRIVATE VM_IOT_ENABLE_LIBPAG=0)
    message(STATUS "vm_iot: libpag disabled")
endif ()

# ---- CLI 工具 iotcamctl ----
# 独立瘦客户端，仅依赖 libc/libstdc++，与 daemon 通过 FIFO 协议通信。
# 故意不放进上面的 src-files glob：CLI 自带 main，与 daemon 互斥。
add_executable(vm_iot_ctl
        ${CMAKE_SOURCE_DIR}/src/cli/iotcamctl.cpp)

# ---- 调试工具 probe_dev ----
# Linux 设备能力手动验证工具：跑 ./probe_dev /dev/video0 与
# `v4l2-ctl --list-formats-ext` 输出对比。仅依赖 v4l2_prober + caps_ranker。
add_executable(probe_dev
        ${CMAKE_SOURCE_DIR}/tests/tools/probe_dev.cpp
        ${CMAKE_SOURCE_DIR}/src/util/v4l2_prober.cpp
        ${CMAKE_SOURCE_DIR}/src/util/caps_ranker.cpp)
target_include_directories(probe_dev PRIVATE
        ${CMAKE_SOURCE_DIR}/src/util
        ${CMAKE_SOURCE_DIR}/src/common)
target_link_libraries(probe_dev PRIVATE spdlog::spdlog)

# ---- 安装规则（make install / cpack 时使用）----
install(TARGETS vm_iot vm_iot_ctl RUNTIME DESTINATION bin)

# ---- 资源同步 ----
# 把仓库根的运行期资源整体 symlink 到二进制目录，方便从 build/ 直接跑可执行文件。
# 所有运行期资源（config / pag / shaders）已物理收敛到顶层 assets/ 目录下，
# 只需链一个 assets 软链即可，子目录通过 cfg.config_dir/.. 路径自然解析到位。
# 注意：CMake -E create_symlink 在 Windows 需要管理员权限，当前目标平台为 Linux 不受影响。
add_custom_command(TARGET vm_iot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
        $<TARGET_FILE_DIR:vm_iot>/assets
        COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:vm_iot>/assets
        COMMENT "Linking assets/ -> $<TARGET_FILE_DIR:vm_iot>/assets (symlink)"
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
            GIT_TAG v1.14.0
    )
    # 防止 GoogleTest 覆盖父项目的编译选项
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    find_package(GTest REQUIRED)
    include_directories(${GTEST_INCLUDE_DIRS})
    add_subdirectory(tests)
endif ()