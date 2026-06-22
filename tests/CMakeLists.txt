# tests/CMakeLists.txt
# 单元测试构建脚本：
#   - 自动遍历当前目录下所有 test_*.cpp，每个文件编译为一个独立可执行文件
#   - 使用 GoogleTest，由 gtest_discover_tests 自动把每个 TEST(...) 注册为 CTest 用例
#   - 新增测试时只需在本目录新增 test_xxx.cpp，无需修改本文件
#
# 共享的被测源码集合（按需追加）。
# 主程序的 main.cpp 不能放进来，否则会和 gtest_main 冲突。
# pag_sdk.cpp 通过 VM_IOT_ENABLE_LIBPAG=0 走 stub 分支，无 libpag 外部依赖；
# gstpagfilter.cpp 是 element 实现，test_pagfilter.cpp 直接调它的注册函数。
set(VM_IOT_TEST_SRCS
        ${CMAKE_SOURCE_DIR}/src/app/config.cpp
        ${CMAKE_SOURCE_DIR}/src/util/v4l2_prober.cpp
        ${CMAKE_SOURCE_DIR}/src/util/caps_ranker.cpp
        ${CMAKE_SOURCE_DIR}/src/pipeline/pipeline_builder.cpp
        ${CMAKE_SOURCE_DIR}/src/plugins/pagfilter/gstpagfilter.cpp
        ${CMAKE_SOURCE_DIR}/src/plugins/pagfilter/pag_sdk.cpp
        ${CMAKE_SOURCE_DIR}/src/plugins/pagfilter/pag_blend.cpp
)

# pipeline_builder.cpp 包含 <gst/gst.h>，需要 GStreamer 的 include / link。
# pagfilter 另外依赖 gstreamer-base（GstBaseTransform）与 gstreamer-video
# （Stage 4.3 起 transform_ip 用 GstVideoFrame 取 plane 指针/stride）。
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST_TEST REQUIRED IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-base-1.0
        gstreamer-video-1.0)

# 共享的 include 目录与外部依赖
set(VM_IOT_TEST_INCLUDES
        ${CMAKE_SOURCE_DIR}/src/app
        ${CMAKE_SOURCE_DIR}/src/common
        ${CMAKE_SOURCE_DIR}/src/pipeline
        ${CMAKE_SOURCE_DIR}/src/rtsp
        ${CMAKE_SOURCE_DIR}/src/util
        ${CMAKE_SOURCE_DIR}/src/plugins/pagfilter
)

set(VM_IOT_TEST_LIBS
        yaml-cpp
        spdlog::spdlog
        PkgConfig::GST_TEST
        GTest::gtest_main
)

include(GoogleTest)

# 自动收集所有 test_*.cpp
file(GLOB VM_IOT_TEST_FILES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

foreach (test_src IN LISTS VM_IOT_TEST_FILES)
    # 文件名（去扩展名）作为可执行文件名，例如 test_config
    get_filename_component(test_name ${test_src} NAME_WE)

    add_executable(${test_name}
            ${test_src}
            ${VM_IOT_TEST_SRCS})

    target_include_directories(${test_name} PRIVATE
            ${VM_IOT_TEST_INCLUDES})

    target_link_libraries(${test_name} PRIVATE
            ${VM_IOT_TEST_LIBS})

    # 单测固定走 pag_sdk 的 stub 分支：CI 主机无需安装 libpag / freetype / EGL / GLES。
    # 真集成分支（VM_IOT_ENABLE_LIBPAG=ON）由主可执行文件 vm_iot 独立验证。
    target_compile_definitions(${test_name} PRIVATE VM_IOT_ENABLE_LIBPAG=0)

    # 自动发现并注册测试用例到 CTest
    gtest_discover_tests(${test_name})
endforeach ()
