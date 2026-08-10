cmake_minimum_required(VERSION 3.25)

project(
    IOCPPractice
    VERSION 1.0.0
    DESCRIPTION "C++17 Windows IOCP learning workspace"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    set(CMAKE_MSVC_OUTPUT_ENCODING "UTF-8")
    set(CMAKE_UTF8_CODEPAGE ON)
endif()

set(IOCP_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(IOCP_PRACTICE_DIR "${IOCP_SOURCE_DIR}/practice")
set(IOCP_RUNTIME_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")

option(IOCP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(CTest)
include("${IOCP_SOURCE_DIR}/cmake/IOCPPracticeHelpers.cmake")

iocp_add_practice_projects()
