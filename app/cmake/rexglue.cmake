# Project-owned integration: codegen may overwrite generated/rexglue.cmake.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Switch ReXGlue source tree")
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
include("${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sources.cmake")
set(SR_GENERATED_SOURCES ${GENERATED_SOURCES})

function(rexglue_setup_target target_name)
    add_library(${target_name}_recomp OBJECT ${SR_GENERATED_SOURCES})
    target_include_directories(${target_name}_recomp PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/default")
    target_link_libraries(${target_name}_recomp PRIVATE rex::runtime)
    if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target_name}_recomp PRIVATE -fasync-exceptions)
    elseif(MSVC)
        target_compile_options(${target_name}_recomp PRIVATE /EHa)
    endif()
    rexglue_apply_target_settings(${target_name}_recomp)
    if(REXGLUE_PLATFORM_SWITCH)
        target_compile_options(${target_name}_recomp PRIVATE
            -Werror=attributes -include "${CMAKE_CURRENT_SOURCE_DIR}/src/sr_recomp_compat.h")
    endif()
    target_precompile_headers(${target_name}_recomp PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/superman_returns_pch.h")
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/default")
    target_link_libraries(${target_name} PRIVATE ${target_name}_recomp rex::runtime)
    rexglue_configure_target(${target_name} ${ARGN})
endfunction()
