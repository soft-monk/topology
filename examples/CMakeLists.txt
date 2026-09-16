# examples/ · 可跑示例（各自独立可执行 + 独立退出码）
#
# 示例同样需要**外部注入的规则包**（TPL-NFR-01 / P7）：路径由 CMake 以绝对路径注入，
# 不依赖当前工作目录。

add_executable(example_minimal minimal/main.cc)
target_link_libraries(example_minimal PRIVATE topology)

add_executable(example_full_flow full_flow/main.cc)
target_link_libraries(example_full_flow PRIVATE topology)

foreach(tgt example_minimal example_full_flow)
  set_target_properties(${tgt} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /utf-8 /W4 /wd4819)
    target_compile_definitions(${tgt} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra)
  endif()
endforeach()

set(TOPOLOGY_POLICY_FILE "${CMAKE_SOURCE_DIR}/policies/mapapp/linkThresholds.json")
target_compile_definitions(example_minimal PRIVATE
  TOPOLOGY_POLICY_FILE="${TOPOLOGY_POLICY_FILE}")
target_compile_definitions(example_full_flow PRIVATE
  TOPOLOGY_POLICY_FILE="${TOPOLOGY_POLICY_FILE}")
