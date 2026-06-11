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

file(GLOB src-files
        ${CMAKE_SOURCE_DIR}/src/*.cpp
        ${CMAKE_SOURCE_DIR}/src/rtsp/*.cpp
        ${CMAKE_SOURCE_DIR}/src/app/*.cpp)

add_executable(vm_iot
        ${src-files})

target_include_directories(vm_iot
        PUBLIC
        src/rtsp
        src/app
        src/common
)


target_link_libraries(vm_iot PUBLIC
        PkgConfig::GST yaml-cpp spdlog::spdlog Threads::Threads)

# ---- CLI 工具 iotcamctl ----
# 独立瘦客户端，仅依赖 libc/libstdc++，与 daemon 通过 FIFO 协议通信。
# 故意不放进上面的 src-files glob：CLI 自带 main，与 daemon 互斥。
add_executable(vm_iot_ctl
        ${CMAKE_SOURCE_DIR}/src/cli/iotcamctl.cpp)

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