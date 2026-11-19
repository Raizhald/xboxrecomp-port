cmake_minimum_required(VERSION 3.20)
project(nv2a_surface_hazard_regression C)
enable_testing()
add_executable(nv2a-surface-hazard-test nv2a_surface_hazard_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c)
target_include_directories(nv2a-surface-hazard-test PRIVATE ../../src/kernel ../../src)
if(MSVC)
    target_compile_definitions(nv2a-surface-hazard-test PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(NOT WIN32)
    target_link_libraries(nv2a-surface-hazard-test PRIVATE m pthread)
endif()
add_test(NAME nv2a-surface-hazard COMMAND nv2a-surface-hazard-test 4)
add_test(NAME nv2a-surface-hazard-sixteen COMMAND nv2a-surface-hazard-test 16)
