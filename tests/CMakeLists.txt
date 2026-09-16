# tests/ · 零依赖自测（一个可执行 + 手写断言）
#
# 覆盖需求专篇 34 条（TPL-MODEL 6 / TPL-Q 6 / TPL-ST 6 / TPL-EVAL 4 / TPL-OPT 6 / TPL-NFR 6）
# 与 §7 验收清单 16 行，**不需要任何外部服务、不需要网络、不依赖真实时间**
# （时钟全部由假时钟注入，TPL-NFR-03）。
#
# 运行： selftest            → 跑全部
#        selftest --list     → 只列用例名
#        selftest --json     → 机检输出（acceptance.ps1 读它做需求↔用例对账）
#        selftest <名字片段>  → 只跑名字里含该片段的用例

add_executable(selftest selftest.cc)
target_link_libraries(selftest PRIVATE topology)

set_target_properties(selftest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(MSVC)
  # /wd4819：源文件是 UTF-8，但 MSVC 在控制台代码页被切到 UTF-8（见 selftest 的 chcp 65001）
  # 时会报"当前代码页(0)"—— 属环境噪声，非源码问题。
  target_compile_options(selftest PRIVATE /utf-8 /W4 /wd4819)
  target_compile_definitions(selftest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(selftest PRIVATE -Wall -Wextra)
endif()

# 夹具与规则包目录：自测通过编译期绝对路径拿到，**不依赖当前工作目录**（TPL-NFR-05）
target_compile_definitions(selftest PRIVATE
  TOPOLOGY_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  TOPOLOGY_POLICY_FILE="${CMAKE_SOURCE_DIR}/policies/mapapp/linkThresholds.json")

add_test(NAME topology_selftest COMMAND selftest)
