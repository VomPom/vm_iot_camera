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
# 构建后把项目根的 config/ 复制到可执行文件输出目录，
# 这样从产物所在目录直接运行时，相对路径 config/default.yaml 即可命中。
# copy_directory_if_different: 仅在源文件较新时才拷贝，避免每次都触发更新。
add_custom_command(TARGET vm_iot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                ${CMAKE_SOURCE_DIR}/config
                $<TARGET_FILE_DIR:vm_iot>/config
        COMMENT "Syncing config/ to $<TARGET_FILE_DIR:vm_iot>/config"
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