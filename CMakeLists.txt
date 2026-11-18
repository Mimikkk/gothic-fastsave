cmake_minimum_required(VERSION 3.21)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(zFastSaveLoad CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(GENERATE_UNION_API_LIB ON CACHE BOOL "" FORCE)
set(GOTHIC_API_G1 ON CACHE BOOL "" FORCE)
add_subdirectory(deps/union-api)
add_subdirectory(deps/gothic-api)

add_library(zFastSaveLoad SHARED src/Plugin.cpp)
target_link_libraries(zFastSaveLoad PRIVATE gothic_api union_api_lib)
target_compile_options(zFastSaveLoad PRIVATE /bigobj /W3 /wd4005 /wd4996 /utf-8)
target_compile_definitions(zFastSaveLoad PRIVATE _CRT_SECURE_NO_WARNINGS)
