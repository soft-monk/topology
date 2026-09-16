# topology · 链路拓扑引擎（独立构建）
#
# 需求依据：docs/需求/topology需求专篇.md
#   TPL-NFR-01（零外部依赖）/ TPL-NFR-05（独立构建、独立示例、独立测试、独立验收脚本）
#
# 目标：
#   topology        静态库（宿主唯一依赖）
#   example_*       examples/ 下的示例
#   selftest        tests/ 下的零依赖自测（注册为 ctest 用例）
#
# 依赖：仅 nlohmann/json（优先系统包，找不到时回落内置单头 third_party/nlohmann/json.hpp）。
# 本模块**永远不需要** Web 框架 / SQL / 其它模块（P1/P2/P3）。
cmake_minimum_required(VERSION 3.20)
project(topology
  VERSION 0.1.0
  DESCRIPTION "链路拓扑引擎：拓扑模型 / 质量聚合 / 状态机与迟滞 / 网络评估 / 优化建议 / 曲线"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# ---------------------------------------------------------------- 选项
option(TOPOLOGY_BUILD_TESTS    "编译 tests/（零依赖自测）" ON)
option(TOPOLOGY_BUILD_EXAMPLES "编译 examples/（最小示例、全流程示例）" ON)
option(TOPOLOGY_WARNINGS_AS_ERRORS "把编译告警当错误" OFF)

# ---------------------------------------------------------------- 依赖
find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
  set(TOPOLOGY_JSON_TARGET nlohmann_json::nlohmann_json)
  message(STATUS "[topology] nlohmann/json: 使用已安装的包")
else()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    message(FATAL_ERROR
      "[topology] 未找到 nlohmann/json：既没有安装包，也没有内置副本 "
      "third_party/nlohmann/json.hpp。请安装 nlohmann-json 或放入内置副本。")
  endif()
  add_library(topology_nlohmann INTERFACE)
  target_include_directories(topology_nlohmann INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  set(TOPOLOGY_JSON_TARGET topology_nlohmann)
  message(STATUS "[topology] nlohmann/json: 使用内置单头副本 third_party/nlohmann/json.hpp")
endif()

# ---------------------------------------------------------------- 库
file(GLOB_RECURSE TOPOLOGY_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc")

add_library(topology STATIC ${TOPOLOGY_SOURCES})
add_library(topology::topology ALIAS topology)

target_include_directories(topology
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(topology PUBLIC ${TOPOLOGY_JSON_TARGET})

target_compile_definitions(topology PUBLIC
  TOPOLOGY_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(topology PRIVATE /utf-8 /W4 /permissive-)
  target_compile_definitions(topology PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    _CRT_SECURE_NO_WARNINGS)
  if(TOPOLOGY_WARNINGS_AS_ERRORS)
    target_compile_options(topology PRIVATE /WX)
  endif()
else()
  target_compile_options(topology PRIVATE -Wall -Wextra)
  if(TOPOLOGY_WARNINGS_AS_ERRORS)
    target_compile_options(topology PRIVATE -Werror)
  endif()
endif()

set_target_properties(topology PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------- 示例 / 测试
if(TOPOLOGY_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(TOPOLOGY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- 汇总
message(STATUS "[topology] ${PROJECT_NAME} ${PROJECT_VERSION} · C++${CMAKE_CXX_STANDARD}")
message(STATUS "[topology]   构建类型 : ${CMAKE_BUILD_TYPE}")
message(STATUS "[topology]   examples/tests : "
               "${TOPOLOGY_BUILD_EXAMPLES}/${TOPOLOGY_BUILD_TESTS}")
