cmake_minimum_required(VERSION 3.16)
project(open_shacccg LANGUAGES C CXX)

option(OPENSHACCG_BUILD_TESTS "Build host tests" ON)
option(OPENSHACCG_BUILD_VITA_STATIC "Build Vita static libshacccg.a" OFF)
option(OPENSHACCG_ENABLE_GLSLANG "Enable glslang frontend adapter" OFF)
option(OPENSHACCG_ENABLE_SPIRV_CROSS "Enable SPIRV-Cross backend adapter" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(OPENSHACCG_CORE_SOURCES
    src/core/allocator.cpp
    src/core/compiler.cpp
    src/frontend/cg_compat.cpp
    src/spirv/spirv_reader.cpp
    src/backend/backend.cpp
    src/backend/machine_ir.cpp
    src/backend/program_builder.cpp
    src/backend/vita_ir.cpp
    src/gxp/gxp_writer.cpp
    src/gxp/gxp_reader.cpp
    src/usse/usse.cpp
)

if(OPENSHACCG_BUILD_VITA_STATIC)
    if(NOT CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi")
        message(FATAL_ERROR "OPENSHACCG_BUILD_VITA_STATIC requires the VitaSDK toolchain")
    endif()

    # Vita consumes this project as a normal static C/C++ library.  Keeping all
    # project objects in one archive makes libshacccg.a usable without the old
    # SUPRX/module link step; the final application link supplies newlib,
    # libstdc++ and libgcc through the regular VitaSDK toolchain.
    add_library(shacccg STATIC
        ${OPENSHACCG_CORE_SOURCES}
        src/abi/sce_shacccg.cpp
    )
    target_include_directories(shacccg PUBLIC include PRIVATE src)
    target_compile_definitions(shacccg PRIVATE
        $<$<BOOL:${OPENSHACCG_ENABLE_GLSLANG}>:OPENSHACCG_ENABLE_GLSLANG=1>
        $<$<BOOL:${OPENSHACCG_ENABLE_SPIRV_CROSS}>:OPENSHACCG_ENABLE_SPIRV_CROSS=1>
    )
    set_target_properties(shacccg PROPERTIES OUTPUT_NAME shacccg)
else()
    add_library(openshacccg_core STATIC ${OPENSHACCG_CORE_SOURCES})
    target_include_directories(openshacccg_core PUBLIC include PRIVATE src)
    target_compile_definitions(openshacccg_core PRIVATE
        $<$<BOOL:${OPENSHACCG_ENABLE_GLSLANG}>:OPENSHACCG_ENABLE_GLSLANG=1>
        $<$<BOOL:${OPENSHACCG_ENABLE_SPIRV_CROSS}>:OPENSHACCG_ENABLE_SPIRV_CROSS=1>
    )

    add_library(openshacccg_abi STATIC src/abi/sce_shacccg.cpp)
    target_include_directories(openshacccg_abi PUBLIC include PRIVATE src)
    target_link_libraries(openshacccg_abi PUBLIC openshacccg_core)
endif()

if(OPENSHACCG_BUILD_TESTS AND NOT OPENSHACCG_BUILD_VITA_STATIC)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    add_executable(openshacccg_tests
        tests/test_main.cpp
        tests/test_abi.cpp
        tests/test_compiler.cpp
        tests/test_spirv.cpp
        tests/test_gxp.cpp
        tests/test_machine_ir.cpp
        tests/test_usse.cpp
    )
    target_link_libraries(openshacccg_tests PRIVATE openshacccg_abi)
    target_include_directories(openshacccg_tests PRIVATE src)
    target_compile_definitions(openshacccg_tests PRIVATE OPENSHACCG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME openshacccg_tests COMMAND openshacccg_tests)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME usse_codegen_check
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_usse_raw.py --check
        )
    endif()
endif()
