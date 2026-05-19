cmake_minimum_required(VERSION 3.20)
project(RemoteControl VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(/utf-8)

option(USE_OPUS "Use Opus audio codec instead of AAC" OFF)

add_subdirectory(Common)
add_subdirectory(Agent)
add_subdirectory(Viewer)
