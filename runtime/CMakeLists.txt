# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(nxrt LANGUAGES C CXX)

option(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER
    "Compile the opt-in guest memory read observer used by compute readback diagnostics"
    OFF
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The game side comes from the static recompiler's export, built once:
#   NXRT_GAME_EXPORT   the export's exefs/ directory (has recomp_registration.c)
#   NXRT_GAME_LIBS     the compiled module archives, in any order, plus the
#                      shared recompiler runtime built with SUYU_HOSTED_RECOMP
set(NXRT_GAME_EXPORT "" CACHE PATH "Recompiler export exefs/ directory")
set(NXRT_GAME_LIBS "" CACHE STRING "Compiled recompiled-module archives (;-separated)")
if(NOT NXRT_GAME_EXPORT OR NOT NXRT_GAME_LIBS)
    message(FATAL_ERROR "set NXRT_GAME_EXPORT and NXRT_GAME_LIBS (see README)")
endif()

add_library(nxrt_core STATIC
    src/log.cpp
    src/memory.cpp
    src/loader.cpp
    src/cpu.cpp
    src/kernel/kernel.cpp
    src/kernel/svc.cpp
    src/hle/ipc.cpp
    src/tts_trace.cpp
    src/guest_sampler.cpp
    src/hle/hooks.cpp
    src/nvn/nvn_layer.cpp
    src/nvn/recorder.cpp
    src/replay/capture_format.cpp
    src/replay/capturing_renderer.cpp
    src/gpu/null_gpu.cpp
    src/frontend/window.cpp
    src/render/vk_context.cpp
    src/render/maxwell.cpp
    src/render/nvn_renderer.cpp
    src/services/services.cpp
    src/services/misc.cpp
    src/services/am.cpp
    src/services/applets.cpp
    src/services/time.cpp
    src/services/settings.cpp
    src/services/fs.cpp
    src/services/acc.cpp
    src/services/nvdrv.cpp
    src/services/vi.cpp
    src/services/hid.cpp
    src/services/input_script.cpp
    src/services/input_record.cpp
    src/services/mii.cpp
    src/services/mii_data/mii_data.cpp
    src/services/audio.cpp
    src/audio/renderer.cpp
    src/audio/output.cpp
    src/services/pl.cpp
)
target_include_directories(nxrt_core PUBLIC src)
if(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    target_compile_definitions(nxrt_core PRIVATE NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER=1)
endif()

# CPU fallback for ASTC textures on hosts whose Vulkan device lacks ASTC.
# Vendored from Arm astc-encoder 5.7.0; only its decompressor is built.
set(ASTCENC_DECOMPRESSOR ON CACHE BOOL "" FORCE)
set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNITTEST OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NONE OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SSE2 OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NEON OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(ASTCENC_ISA_SSE2 ON CACHE BOOL "" FORCE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(ASTCENC_ISA_NEON ON CACHE BOOL "" FORCE)
else()
    set(ASTCENC_ISA_NONE ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(third_party/astc-encoder EXCLUDE_FROM_ALL)
if(ASTCENC_ISA_SSE2)
    target_link_libraries(nxrt_core PUBLIC astcdec-sse2-static)
elseif(ASTCENC_ISA_NEON)
    target_link_libraries(nxrt_core PUBLIC astcdec-neon-static)
else()
    target_link_libraries(nxrt_core PUBLIC astcdec-none-static)
endif()

# Maxwell shader -> SPIR-V compiler reused from suyu (GPL-2.0-or-later), with
# sirit (BSD-3-Clause) to emit SPIR-V. A compiler library, nothing of the
# emulator; meant to be replaced by nxrt's own. Needs Boost headers
# (container, intrusive): NXRT_BOOST_ROOT points at a Boost source tree.
set(NXRT_BOOST_ROOT "" CACHE PATH "Boost source tree (libs/*/include) or include dir")
set(CMAKE_POLICY_VERSION_MINIMUM 3.5) # sirit and SPIRV-Headers predate CMake 4
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)
add_library(shader_recompiler STATIC
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_barriers.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_bitwise_conversion.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_composite.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_control_flow.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_convert.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_floating_point.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_image.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_image_atomic.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_integer.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_logical.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_memory.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_select.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_shared_memory.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_special.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_undefined.cpp
    third_party/suyu/shader_recompiler/backend/spirv/emit_spirv_warp.cpp
    third_party/suyu/shader_recompiler/backend/spirv/spirv_emit_context.cpp
    third_party/suyu/shader_recompiler/frontend/ir/basic_block.cpp
    third_party/suyu/shader_recompiler/frontend/ir/condition.cpp
    third_party/suyu/shader_recompiler/frontend/ir/ir_emitter.cpp
    third_party/suyu/shader_recompiler/frontend/ir/microinstruction.cpp
    third_party/suyu/shader_recompiler/frontend/ir/opcodes.cpp
    third_party/suyu/shader_recompiler/frontend/ir/patch.cpp
    third_party/suyu/shader_recompiler/frontend/ir/post_order.cpp
    third_party/suyu/shader_recompiler/frontend/ir/program.cpp
    third_party/suyu/shader_recompiler/frontend/ir/type.cpp
    third_party/suyu/shader_recompiler/frontend/ir/value.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/control_flow.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/decode.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/indirect_branch_table_track.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/opcodes.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/structured_control_flow.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/atomic_operations_global_memory.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/atomic_operations_shared_memory.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/attribute_memory_to_physical.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/barrier_operations.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/bitfield_extract.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/bitfield_insert.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/branch_indirect.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/common_funcs.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/condition_code_set.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/double_add.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/double_compare_and_set.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/double_fused_multiply_add.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/double_min_max.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/double_multiply.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/double_set_predicate.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/exit_program.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/find_leading_one.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/floating_point_add.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/floating_point_compare.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/floating_point_compare_and_set.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/floating_point_conversion_floating_point.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/floating_point_conversion_integer.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/floating_point_fused_multiply_add.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/floating_point_min_max.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/floating_point_multi_function.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/floating_point_multiply.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/floating_point_range_reduction.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/floating_point_set_predicate.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/floating_point_swizzled_add.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_add.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_fused_multiply_add.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_helper.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_multiply.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_set.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_set_predicate.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/impl.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_add.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_add_three_input.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_compare.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_compare_and_set.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_floating_point_conversion.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_funnel_shift.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_minimum_maximum.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_popcount.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_scaled_add.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_set_predicate.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_shift_left.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_shift_right.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_short_multiply_add.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/integer_to_integer_conversion.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/internal_stage_buffer_entry_read.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/load_constant.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/load_effective_address.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/load_store_attribute.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/load_store_local_shared.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/load_store_memory.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/logic_operation.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/logic_operation_three_input.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/move_predicate_to_register.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/move_register.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/move_register_to_predicate.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/move_special_register.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/not_implemented.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/output_geometry.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/pixel_load.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/predicate_set_predicate.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/predicate_set_register.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/select_source_with_predicate.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/surface_atomic_operations.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/surface_load_store.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/texture_fetch.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/texture_fetch_swizzled.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/texture_gather.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/texture_gather_swizzled.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/texture_gradient.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/texture_load.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/texture_load_swizzled.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/texture_mipmap_level.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/texture_query.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/video_helper.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/video_minimum_maximum.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/video_multiply_add.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/video_set_predicate.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/vote.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/impl/warp_shuffle.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate/translate.cpp
    third_party/suyu/shader_recompiler/frontend/maxwell/translate_program.cpp
    third_party/suyu/shader_recompiler/ir_opt/collect_shader_info_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/conditional_barrier_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/constant_propagation_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/dead_code_elimination_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/dual_vertex_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/identity_removal_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/layer_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/lower_fp16_to_fp32.cpp
    third_party/suyu/shader_recompiler/ir_opt/lower_fp64_to_fp32.cpp
    third_party/suyu/shader_recompiler/ir_opt/lower_int64_to_int32.cpp
    third_party/suyu/shader_recompiler/ir_opt/position_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/rescaling_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/ssa_rewrite_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/texture_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/vendor_workaround_pass.cpp
    third_party/suyu/shader_recompiler/ir_opt/verification_pass.cpp
)
target_include_directories(shader_recompiler PUBLIC third_party/suyu third_party/unordered_dense/include)
if(NXRT_BOOST_ROOT)
    file(GLOB NXRT_BOOST_LIB_INCLUDES "${NXRT_BOOST_ROOT}/libs/*/include")
    target_include_directories(shader_recompiler SYSTEM PUBLIC ${NXRT_BOOST_LIB_INCLUDES} "${NXRT_BOOST_ROOT}")
endif()
find_package(fmt REQUIRED)
target_link_libraries(shader_recompiler PUBLIC fmt::fmt sirit)
target_compile_options(shader_recompiler PRIVATE -w)
# The host code uses GCC builtins (__atomic_*, unsigned __int128, __builtin_bswap32):
# gcc, clang and mingw-w64 only. cl.exe is not supported.
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "nxrt needs GCC or clang (mingw-w64 / clang-cl); cl.exe lacks "
    "__atomic_*, unsigned __int128 and __builtin_* used in src/")
endif()
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  # clang-cl: pass the GNU-style flags through; plain -Wall would mean /Wall (-Weverything).
  target_compile_options(nxrt_core PRIVATE /clang:-Wall /clang:-Wextra /clang:-Wno-unused-parameter /clang:-mcx16)
else()
  target_compile_options(nxrt_core PRIVATE -Wall -Wextra -Wno-unused-parameter -mcx16)
endif()
if(WIN32)
  # windows.h defines min/max macros that break std::min/std::max, and pulls in far more than needed.
  target_compile_definitions(nxrt_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
find_package(Threads REQUIRED)
find_package(SDL2 REQUIRED)
# Khronos Vulkan headers are vendored; the loader (libvulkan) comes from the system.
target_include_directories(nxrt_core PUBLIC third_party/vulkan-headers/include)
if(WIN32)
  # vulkan-1.dll comes with the GPU driver; the import library from the SDK.
  find_package(Vulkan REQUIRED)
  set(NXRT_VULKAN_LIB Vulkan::Vulkan)
else()
  set(NXRT_VULKAN_LIB vulkan)
endif()
target_link_libraries(nxrt_core PUBLIC Threads::Threads SDL2::SDL2 ${NXRT_VULKAN_LIB} shader_recompiler)

add_executable(nxrt src/main.cpp "${NXRT_GAME_EXPORT}/recomp_registration.c")
# install.sh copies libfmt next to the program (../lib): a build made in a container (distrobox) then also runs on
# a host whose fmt is older or missing, such as SteamOS.
set_target_properties(nxrt PROPERTIES BUILD_RPATH "$ORIGIN/../lib")
target_compile_definitions(nxrt PRIVATE NXRT_SOURCE_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/suyu/fonts")
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  # lld-link: static libraries are resolved without ordering, no group needed.
  find_library(NXRT_ATOMIC_LIB atomic)
  if(NXRT_ATOMIC_LIB)
    target_link_libraries(nxrt PRIVATE nxrt_core ${NXRT_GAME_LIBS} ${NXRT_ATOMIC_LIB})
  else()
    target_link_libraries(nxrt PRIVATE nxrt_core ${NXRT_GAME_LIBS})
  endif()
elseif(WIN32)
  # mingw ld understands groups; libatomic is optional (llvm-mingw does not ship it).
  find_library(NXRT_ATOMIC_LIB atomic)
  if(NXRT_ATOMIC_LIB)
    target_link_libraries(nxrt PRIVATE nxrt_core
        -Wl,--start-group ${NXRT_GAME_LIBS} -Wl,--end-group ${NXRT_ATOMIC_LIB})
  else()
    target_link_libraries(nxrt PRIVATE nxrt_core
        -Wl,--start-group ${NXRT_GAME_LIBS} -Wl,--end-group)
  endif()
else()
  target_link_libraries(nxrt PRIVATE nxrt_core
      -Wl,--start-group ${NXRT_GAME_LIBS} -Wl,--end-group atomic)
endif()

add_executable(nxrt-multiview tools/multiview/main.cpp tools/multiview/replay_runtime_stubs.cpp)
target_link_libraries(nxrt-multiview PRIVATE nxrt_core)
if(WIN32)
  if(NXRT_ATOMIC_LIB)
    target_link_libraries(nxrt-multiview PRIVATE ${NXRT_ATOMIC_LIB})
  endif()
else()
  target_link_libraries(nxrt-multiview PRIVATE atomic)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(nxrt_resolution_model_boundary_tests tests/render/resolution_model_boundary_test.cpp)
  target_include_directories(nxrt_resolution_model_boundary_tests PRIVATE src)
  add_test(NAME nxrt_resolution_model_boundary_tests COMMAND nxrt_resolution_model_boundary_tests)
  add_executable(nxrt_model_journal_tests tests/render/nvn_model_journal_test.cpp
      src/nvn/recorder.cpp src/memory.cpp src/log.cpp)
  target_include_directories(nxrt_model_journal_tests PRIVATE src)
  target_link_libraries(nxrt_model_journal_tests PRIVATE fmt::fmt Threads::Threads)
  if(NOT WIN32)
    target_link_libraries(nxrt_model_journal_tests PRIVATE atomic)
  elseif(NXRT_ATOMIC_LIB)
    target_link_libraries(nxrt_model_journal_tests PRIVATE ${NXRT_ATOMIC_LIB})
  endif()
  add_test(NAME nxrt_model_journal_tests COMMAND nxrt_model_journal_tests)
  add_executable(nxrt_execution_model_snapshot_tests tests/render/execution_model_snapshot_test.cpp)
  target_include_directories(nxrt_execution_model_snapshot_tests PRIVATE src)
  add_test(NAME nxrt_execution_model_snapshot_tests COMMAND nxrt_execution_model_snapshot_tests)
  add_executable(nxrt_nvn_texture_binding_tests
      tests/render/nvn_texture_binding_test.cpp
      src/nvn/recorder.cpp src/memory.cpp src/log.cpp
      src/replay/capturing_renderer.cpp src/replay/capture_format.cpp
  )
  target_include_directories(nxrt_nvn_texture_binding_tests PRIVATE src)
  target_compile_options(nxrt_nvn_texture_binding_tests PRIVATE $<TARGET_PROPERTY:nxrt_core,COMPILE_OPTIONS>)
  target_link_libraries(nxrt_nvn_texture_binding_tests PRIVATE fmt::fmt Threads::Threads)
  if(NOT WIN32)
    target_link_libraries(nxrt_nvn_texture_binding_tests PRIVATE atomic)
  elseif(NXRT_ATOMIC_LIB)
    target_link_libraries(nxrt_nvn_texture_binding_tests PRIVATE ${NXRT_ATOMIC_LIB})
  endif()
  add_test(NAME nxrt_nvn_texture_binding_tests COMMAND nxrt_nvn_texture_binding_tests)
  add_executable(nxrt_capture_format_tests
      tests/replay/capture_format_test.cpp
      src/replay/capture_format.cpp
  )
  target_include_directories(nxrt_capture_format_tests PRIVATE src)
  target_link_libraries(nxrt_capture_format_tests PRIVATE Threads::Threads)
  add_test(NAME nxrt_capture_format_tests COMMAND nxrt_capture_format_tests)
  add_executable(nxrt_memory_read_observer_tests
      tests/memory_read_observer_test.cpp
      src/memory.cpp src/log.cpp
  )
  target_include_directories(nxrt_memory_read_observer_tests PRIVATE src)
  target_compile_definitions(nxrt_memory_read_observer_tests PRIVATE NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER=1)
  target_link_libraries(nxrt_memory_read_observer_tests PRIVATE fmt::fmt Threads::Threads)
  if(NOT WIN32)
    target_link_libraries(nxrt_memory_read_observer_tests PRIVATE atomic)
  elseif(NXRT_ATOMIC_LIB)
    target_link_libraries(nxrt_memory_read_observer_tests PRIVATE ${NXRT_ATOMIC_LIB})
  endif()
  add_test(NAME nxrt_memory_read_observer_tests COMMAND nxrt_memory_read_observer_tests)
  add_executable(nxrt_input_script_tests
      tests/input_script_tests.cpp
      src/services/input_script.cpp
  )
  target_include_directories(nxrt_input_script_tests PRIVATE src)
  add_test(NAME nxrt_input_script_tests COMMAND nxrt_input_script_tests)
  add_executable(nxrt_input_record_tests
      tests/input_record_tests.cpp
      src/services/input_record.cpp
      src/services/input_script.cpp
  )
  target_include_directories(nxrt_input_record_tests PRIVATE src)
  add_test(NAME nxrt_input_record_tests COMMAND nxrt_input_record_tests)
  add_executable(nxrt_tts_trace_tests tests/tts_trace_tests.cpp)
  target_include_directories(nxrt_tts_trace_tests PRIVATE src)
  add_test(NAME nxrt_tts_trace_tests COMMAND nxrt_tts_trace_tests)
  add_executable(nxrt_draw_census_tests tests/render/draw_census_test.cpp)
  target_include_directories(nxrt_draw_census_tests PRIVATE src)
  add_test(NAME nxrt_draw_census_tests COMMAND nxrt_draw_census_tests)
  add_executable(nxrt_depth_clear_policy_tests tests/render/depth_clear_policy_test.cpp)
  target_include_directories(nxrt_depth_clear_policy_tests PRIVATE src)
  add_test(NAME nxrt_depth_clear_policy_tests COMMAND nxrt_depth_clear_policy_tests)
  add_executable(nxrt_sampler_border_policy_tests tests/render/sampler_border_policy_test.cpp)
  target_include_directories(nxrt_sampler_border_policy_tests PRIVATE src)
  add_test(NAME nxrt_sampler_border_policy_tests COMMAND nxrt_sampler_border_policy_tests)
  add_executable(nxrt_block_texel_target_tests tests/render/block_texel_target_test.cpp src/render/maxwell.cpp)
  target_include_directories(nxrt_block_texel_target_tests PRIVATE src third_party/vulkan-headers/include)
  target_link_libraries(nxrt_block_texel_target_tests PRIVATE fmt::fmt)
  add_test(NAME nxrt_block_texel_target_tests COMMAND nxrt_block_texel_target_tests)

  add_executable(nxrt_graphics_state_cache_tests tests/render/graphics_state_cache_test.cpp)
  target_include_directories(nxrt_graphics_state_cache_tests PRIVATE src third_party/vulkan-headers/include)
  add_test(NAME nxrt_graphics_state_cache_tests COMMAND nxrt_graphics_state_cache_tests)
  add_executable(nxrt_program_trace_tests tests/render/program_trace_test.cpp)
  target_include_directories(nxrt_program_trace_tests PRIVATE src)
  add_test(NAME nxrt_program_trace_tests COMMAND nxrt_program_trace_tests)
  add_executable(nxrt_vsync_pacing_tests tests/services/vsync_pacing_test.cpp)
  target_include_directories(nxrt_vsync_pacing_tests PRIVATE src)
  add_test(NAME nxrt_vsync_pacing_tests COMMAND nxrt_vsync_pacing_tests)
  add_executable(nxrt_syncpoint_timeline_tests tests/services/syncpoint_timeline_test.cpp)
  target_include_directories(nxrt_syncpoint_timeline_tests PRIVATE src)
  target_link_libraries(nxrt_syncpoint_timeline_tests PRIVATE Threads::Threads)
  add_test(NAME nxrt_syncpoint_timeline_tests COMMAND nxrt_syncpoint_timeline_tests)
  add_executable(nxrt_nvdrv_syncpoint_tests
      tests/services/nvdrv_syncpoint_test.cpp
      tools/multiview/replay_runtime_stubs.cpp
  )
  target_include_directories(nxrt_nvdrv_syncpoint_tests PRIVATE src)
  target_link_libraries(nxrt_nvdrv_syncpoint_tests PRIVATE nxrt_core Threads::Threads)
  add_test(NAME nxrt_nvdrv_syncpoint_tests COMMAND nxrt_nvdrv_syncpoint_tests)
  add_executable(nxrt_null_gpu_snapshot_tests
      tests/gpu/null_gpu_snapshot_test.cpp
      src/gpu/null_gpu.cpp src/memory.cpp src/log.cpp
  )
  target_include_directories(nxrt_null_gpu_snapshot_tests PRIVATE src)
  target_compile_options(nxrt_null_gpu_snapshot_tests PRIVATE $<TARGET_PROPERTY:nxrt_core,COMPILE_OPTIONS>)
  target_link_libraries(nxrt_null_gpu_snapshot_tests PRIVATE fmt::fmt Threads::Threads)
  if(NOT WIN32)
    target_link_libraries(nxrt_null_gpu_snapshot_tests PRIVATE atomic)
  elseif(NXRT_ATOMIC_LIB)
    target_link_libraries(nxrt_null_gpu_snapshot_tests PRIVATE ${NXRT_ATOMIC_LIB})
  endif()
  add_test(NAME nxrt_null_gpu_snapshot_tests COMMAND nxrt_null_gpu_snapshot_tests)
  add_executable(nxrt_mii_data_tests
      tests/mii/mii_data_test.cpp
      src/services/mii_data/mii_data.cpp
  )
  target_include_directories(nxrt_mii_data_tests PRIVATE src)
  add_test(NAME nxrt_mii_data_tests COMMAND nxrt_mii_data_tests)
  add_executable(nxrt_shader_failure_trace_tests
      tests/render/shader_failure_trace_test.cpp
      third_party/suyu/shader_recompiler/frontend/maxwell/decode.cpp
  )
  target_include_directories(nxrt_shader_failure_trace_tests PRIVATE src third_party/suyu)
  target_link_libraries(nxrt_shader_failure_trace_tests PRIVATE fmt::fmt)
  add_test(NAME nxrt_shader_failure_trace_tests COMMAND nxrt_shader_failure_trace_tests)

  add_executable(nxrt_sirit_stream_local_address_tests
      tests/render/sirit_stream_local_address_test.cpp
  )
  target_include_directories(nxrt_sirit_stream_local_address_tests PRIVATE
      third_party/sirit/src
      third_party/sirit/include
      third_party/sirit/externals/SPIRV-Headers/include
  )
  add_test(NAME nxrt_sirit_stream_local_address_tests
      COMMAND nxrt_sirit_stream_local_address_tests)

  add_executable(nxrt_spirv_global_memory_cbuf_types_tests
      tests/render/spirv_global_memory_cbuf_types_test.cpp
  )
  target_include_directories(nxrt_spirv_global_memory_cbuf_types_tests PRIVATE third_party/suyu)
  target_link_libraries(nxrt_spirv_global_memory_cbuf_types_tests PRIVATE shader_recompiler)
  add_test(NAME nxrt_spirv_global_memory_cbuf_types_tests
      COMMAND nxrt_spirv_global_memory_cbuf_types_tests)

  add_executable(nxrt_spirv_position_invariant_probe_tests
      tests/render/spirv_position_invariant_probe_test.cpp
  )
  target_include_directories(nxrt_spirv_position_invariant_probe_tests PRIVATE third_party/suyu)
  target_link_libraries(nxrt_spirv_position_invariant_probe_tests PRIVATE shader_recompiler)
  add_test(NAME nxrt_spirv_position_invariant_probe_tests
      COMMAND nxrt_spirv_position_invariant_probe_tests)

  add_executable(nxrt_spirv_global_memory_ssbo_types_tests
      tests/render/spirv_global_memory_ssbo_types_test.cpp
  )
  target_include_directories(nxrt_spirv_global_memory_ssbo_types_tests PRIVATE third_party/suyu)
  target_link_libraries(nxrt_spirv_global_memory_ssbo_types_tests PRIVATE shader_recompiler)
  add_test(NAME nxrt_spirv_global_memory_ssbo_types_tests
      COMMAND nxrt_spirv_global_memory_ssbo_types_tests)

  add_executable(nxrt_process_memory_status_tests
      tests/render/process_memory_status_test.cpp
  )
  target_include_directories(nxrt_process_memory_status_tests PRIVATE src)
  add_test(NAME nxrt_process_memory_status_tests COMMAND nxrt_process_memory_status_tests)

  add_executable(nxrt_indirect_branch_tracker_tests
      tests/render/indirect_branch_tracker_test.cpp
      third_party/suyu/shader_recompiler/frontend/maxwell/indirect_branch_table_track.cpp
  )
  target_link_libraries(nxrt_indirect_branch_tracker_tests PRIVATE shader_recompiler)
  add_test(NAME nxrt_indirect_branch_tracker_tests COMMAND nxrt_indirect_branch_tracker_tests)

  add_executable(nxrt_inline_keyboard_input_tests tests/inline_keyboard_input_tests.cpp)
  target_include_directories(nxrt_inline_keyboard_input_tests PRIVATE src)
  add_test(NAME nxrt_inline_keyboard_input_tests COMMAND nxrt_inline_keyboard_input_tests)

  add_executable(nxrt_keyboard_game_input_tests tests/keyboard_game_input_tests.cpp)
  target_include_directories(nxrt_keyboard_game_input_tests PRIVATE src)
  add_test(NAME nxrt_keyboard_game_input_tests COMMAND nxrt_keyboard_game_input_tests)

  add_executable(nxrt_hid_touch_tests tests/hid_touch_tests.cpp)
  target_include_directories(nxrt_hid_touch_tests PRIVATE src)
  add_test(NAME nxrt_hid_touch_tests COMMAND nxrt_hid_touch_tests)

  add_executable(nxrt_system_region_tests tests/system_region_tests.cpp)
  target_include_directories(nxrt_system_region_tests PRIVATE src)
  add_test(NAME nxrt_system_region_tests COMMAND nxrt_system_region_tests)

  add_executable(nxrt_inline_keyboard_protocol_tests tests/inline_keyboard_protocol_tests.cpp)
  target_include_directories(nxrt_inline_keyboard_protocol_tests PRIVATE src)
  add_test(NAME nxrt_inline_keyboard_protocol_tests COMMAND nxrt_inline_keyboard_protocol_tests)

  add_executable(nxrt_framebuffer_cache_key_tests tests/render/framebuffer_cache_key_test.cpp)
  target_include_directories(nxrt_framebuffer_cache_key_tests PRIVATE src)
  add_test(NAME nxrt_framebuffer_cache_key_tests COMMAND nxrt_framebuffer_cache_key_tests)

  add_executable(nxrt_pipeline_cache_key_tests tests/render/pipeline_cache_key_test.cpp)
  target_include_directories(nxrt_pipeline_cache_key_tests PRIVATE src)
  add_test(NAME nxrt_pipeline_cache_key_tests COMMAND nxrt_pipeline_cache_key_tests)

  add_executable(nxrt_flush_policy_tests tests/render/flush_policy_test.cpp)
  target_include_directories(nxrt_flush_policy_tests PRIVATE src third_party/vulkan-headers/include)
  add_test(NAME nxrt_flush_policy_tests COMMAND nxrt_flush_policy_tests)

  add_executable(nxrt_gpu_timestamps_tests tests/render/gpu_timestamps_test.cpp)
  target_include_directories(nxrt_gpu_timestamps_tests PRIVATE src)
  add_test(NAME nxrt_gpu_timestamps_tests COMMAND nxrt_gpu_timestamps_tests)

  add_executable(nxrt_win_sample_trace_tests tests/render/win_sample_trace_test.cpp)
  target_include_directories(nxrt_win_sample_trace_tests PRIVATE src)
  add_test(NAME nxrt_win_sample_trace_tests COMMAND nxrt_win_sample_trace_tests)

  add_executable(nxrt_image_row_transform_tests tests/render/image_row_transform_test.cpp)
  target_include_directories(nxrt_image_row_transform_tests PRIVATE src)
  add_test(NAME nxrt_image_row_transform_tests COMMAND nxrt_image_row_transform_tests)

  add_executable(nxrt_descriptor_info_stability_tests
      tests/render/descriptor_info_stability_test.cpp
  )
  target_include_directories(nxrt_descriptor_info_stability_tests PRIVATE
      src
      third_party/vulkan-headers/include
  )
  add_test(NAME nxrt_descriptor_info_stability_tests COMMAND nxrt_descriptor_info_stability_tests)

  add_executable(nxrt_push_descriptor_policy_tests
      tests/render/push_descriptor_policy_test.cpp
  )
  target_include_directories(nxrt_push_descriptor_policy_tests PRIVATE
      src
      third_party/vulkan-headers/include
  )
  add_test(NAME nxrt_push_descriptor_policy_tests COMMAND nxrt_push_descriptor_policy_tests)

  add_executable(nxrt_texture_descriptor_policy_tests
      tests/render/texture_descriptor_policy_test.cpp
  )
  target_include_directories(nxrt_texture_descriptor_policy_tests PRIVATE src)
  add_test(NAME nxrt_texture_descriptor_policy_tests COMMAND nxrt_texture_descriptor_policy_tests)

  add_executable(nxrt_guest_buffer_upload_cache_tests
      tests/render/guest_buffer_upload_cache_test.cpp
  )
  target_include_directories(nxrt_guest_buffer_upload_cache_tests PRIVATE src)
  add_test(NAME nxrt_guest_buffer_upload_cache_tests COMMAND nxrt_guest_buffer_upload_cache_tests)

  add_executable(nxrt_async_writeback_tests
      tests/render/async_writeback_test.cpp
      src/memory.cpp src/log.cpp
  )
  target_include_directories(nxrt_async_writeback_tests PRIVATE src)
  target_link_libraries(nxrt_async_writeback_tests PRIVATE fmt::fmt Threads::Threads)
  if(NOT WIN32)
    target_link_libraries(nxrt_async_writeback_tests PRIVATE atomic)
  elseif(NXRT_ATOMIC_LIB)
    target_link_libraries(nxrt_async_writeback_tests PRIVATE ${NXRT_ATOMIC_LIB})
  endif()
  add_test(NAME nxrt_async_writeback_tests COMMAND nxrt_async_writeback_tests)
  add_executable(nxrt_guest_memory_range_tests
      tests/render/guest_memory_range_test.cpp
  )
  target_include_directories(nxrt_guest_memory_range_tests PRIVATE src)
  add_test(NAME nxrt_guest_memory_range_tests COMMAND nxrt_guest_memory_range_tests)

  add_executable(nxrt_guest_memory_readback_tracker_tests
      tests/render/guest_memory_readback_tracker_test.cpp
  )
  target_include_directories(nxrt_guest_memory_readback_tracker_tests PRIVATE src)
  add_test(NAME nxrt_guest_memory_readback_tracker_tests COMMAND nxrt_guest_memory_readback_tracker_tests)

  add_executable(nxrt_texture_dirty_tracker_tests
      tests/render/texture_dirty_tracker_test.cpp
  )
  target_include_directories(nxrt_texture_dirty_tracker_tests PRIVATE src)
  add_test(NAME nxrt_texture_dirty_tracker_tests COMMAND nxrt_texture_dirty_tracker_tests)

  add_executable(nxrt_direct_mapped_upload_cache_tests
      tests/render/direct_mapped_upload_cache_test.cpp
  )
  target_include_directories(nxrt_direct_mapped_upload_cache_tests PRIVATE src)
  add_test(NAME nxrt_direct_mapped_upload_cache_tests COMMAND nxrt_direct_mapped_upload_cache_tests)

  add_executable(nxrt_compute_writeback_policy_tests
      tests/render/compute_writeback_policy_test.cpp
  )
  target_include_directories(nxrt_compute_writeback_policy_tests PRIVATE src)
  add_test(NAME nxrt_compute_writeback_policy_tests COMMAND nxrt_compute_writeback_policy_tests)

  add_executable(nxrt_execute_lock_scope_tests
      tests/render/execute_lock_scope_test.cpp
  )
  target_include_directories(nxrt_execute_lock_scope_tests PRIVATE src)
  target_link_libraries(nxrt_execute_lock_scope_tests PRIVATE Threads::Threads)
  add_test(NAME nxrt_execute_lock_scope_tests COMMAND nxrt_execute_lock_scope_tests)
  set_tests_properties(nxrt_execute_lock_scope_tests PROPERTIES TIMEOUT 30)

  add_executable(nxrt_serial_renderer_queue_tests
      tests/render/serial_renderer_queue_test.cpp
  )
  target_include_directories(nxrt_serial_renderer_queue_tests PRIVATE src)
  target_link_libraries(nxrt_serial_renderer_queue_tests PRIVATE Threads::Threads)
  add_test(NAME nxrt_serial_renderer_queue_tests COMMAND nxrt_serial_renderer_queue_tests)
  set_tests_properties(nxrt_serial_renderer_queue_tests PROPERTIES TIMEOUT 30)

  add_executable(nxrt_win32_shared_memory_lifecycle_tests
      tests/platform/win32_shared_memory_lifecycle_test.cpp
  )
  target_include_directories(nxrt_win32_shared_memory_lifecycle_tests BEFORE PRIVATE
      tests/platform/fake_windows src
  )
  target_compile_definitions(nxrt_win32_shared_memory_lifecycle_tests PRIVATE _WIN32)
  add_test(NAME nxrt_win32_shared_memory_lifecycle_tests
      COMMAND nxrt_win32_shared_memory_lifecycle_tests)
endif()

# Opt-in benchmark/test of the address arbiter (not part of CTest's timing-sensitive set).
if(BUILD_TESTING AND NOT WIN32)
  add_executable(nxrt_sync_bench tests/sync_bench.cpp tools/multiview/replay_runtime_stubs.cpp)
  target_link_libraries(nxrt_sync_bench PRIVATE nxrt_core atomic)
endif()
