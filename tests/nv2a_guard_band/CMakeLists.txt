cmake_minimum_required(VERSION 3.20)
project(nv2a_guard_band_regression C)
enable_testing()
add_executable(nv2a-guard-band-test nv2a_guard_band_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_backend_d3d11.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c)
target_include_directories(nv2a-guard-band-test PRIVATE ../../src/kernel ../../src)
if(MSVC)
    target_compile_definitions(nv2a-guard-band-test PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(NOT WIN32)
    target_link_libraries(nv2a-guard-band-test PRIVATE m pthread)
endif()
add_test(NAME nv2a-guard-band COMMAND nv2a-guard-band-test)
