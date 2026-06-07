cmake_minimum_required(VERSION 4.2)
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

# ---- 资源同步 ----
# 构建后在可执行文件输出目录创建一个指向项目根 config/ 的符号链接，
# 这样运行时打开的就是源文件本身：编辑 config/*.yaml 后无需重新构建，
# 重启进程即可生效（彻底解决"改了配置不生效需要重新 build"的问题）。
#
# 步骤：
# 1) 先尝试删除已有的链接/目录（rm -f 对不存在的路径不会报错），
#    避免历史遗留的真实拷贝目录或失效软链阻塞 create_symlink；
# 2) 重新创建软链：$<TARGET_FILE_DIR:vm_iot>/config -> ${CMAKE_SOURCE_DIR}/config
add_custom_command(TARGET vm_iot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
                $<TARGET_FILE_DIR:vm_iot>/config
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                ${CMAKE_SOURCE_DIR}/config
                $<TARGET_FILE_DIR:vm_iot>/config
        COMMENT "Linking config/ -> $<TARGET_FILE_DIR:vm_iot>/config (symlink)"
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
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif ()