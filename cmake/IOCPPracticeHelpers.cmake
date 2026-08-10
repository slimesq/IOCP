function(iocp_configure_target target_name)
    target_compile_options(${target_name} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
        $<$<CXX_COMPILER_ID:MSVC>:/EHsc>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wpedantic>
    )

    if(IOCP_WARNINGS_AS_ERRORS)
        target_compile_options(${target_name} PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/WX>
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        )
    endif()

    if(WIN32)
        target_compile_definitions(${target_name} PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
        )
        target_link_libraries(${target_name} PRIVATE
            ws2_32
            mswsock
        )
    endif()

    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${IOCP_RUNTIME_OUTPUT_DIR}"
    )

    file(RELATIVE_PATH target_folder
        "${IOCP_PRACTICE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    string(REPLACE "\\" "/" target_folder "${target_folder}")
    set_target_properties(${target_name} PROPERTIES FOLDER "practice/${target_folder}")

    if(BUILD_TESTING)
        add_test(NAME "${target_name}.Smoke" COMMAND ${target_name})
    endif()
endfunction()

function(iocp_make_practice_target_name output_variable source_directory)
    file(RELATIVE_PATH relative_path
        "${IOCP_PRACTICE_DIR}"
        "${source_directory}"
    )
    string(TOLOWER "${relative_path}" normalized_name)
    string(REGEX REPLACE "[^a-z0-9]+" "_" normalized_name "${normalized_name}")
    string(REGEX REPLACE "^_+|_+$" "" normalized_name "${normalized_name}")

    if(NOT normalized_name)
        message(FATAL_ERROR "Could not create a target name for: ${source_directory}")
    endif()

    set(${output_variable} "iocp_${normalized_name}" PARENT_SCOPE)
endfunction()

function(iocp_collect_current_sources output_variable)
    set(source_patterns)
    foreach(extension IN ITEMS cpp cc cxx h hpp hh hxx)
        list(APPEND source_patterns "${CMAKE_CURRENT_SOURCE_DIR}/*.${extension}")
    endforeach()

    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
        ${source_patterns}
    )
    list(FILTER sources EXCLUDE REGEX "[/\\\\](build|out|\\.git)[/\\\\]")
    if(NOT sources)
        message(FATAL_ERROR "No C++ source files found in: ${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    set(${output_variable} ${sources} PARENT_SCOPE)
endfunction()

function(iocp_add_current_practice_executable)
    iocp_make_practice_target_name(target_name "${CMAKE_CURRENT_SOURCE_DIR}")
    iocp_collect_current_sources(sources)

    add_executable(${target_name} ${sources})
    iocp_configure_target(${target_name})
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${sources})

    set(IOCP_CURRENT_TARGET "${target_name}" PARENT_SCOPE)
    message(STATUS "Added IOCP practice target: ${target_name}")
endfunction()

function(iocp_add_child_projects)
    file(GLOB child_cmake_files CONFIGURE_DEPENDS
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt"
    )

    foreach(child_cmake_file IN LISTS child_cmake_files)
        get_filename_component(child_directory "${child_cmake_file}" DIRECTORY)
        add_subdirectory("${child_directory}")
    endforeach()
endfunction()

function(iocp_add_practice_projects)
    file(GLOB practice_cmake_files CONFIGURE_DEPENDS
        RELATIVE "${IOCP_PRACTICE_DIR}"
        "${IOCP_PRACTICE_DIR}/*/CMakeLists.txt"
    )

    foreach(practice_cmake_file IN LISTS practice_cmake_files)
        get_filename_component(practice_directory "${practice_cmake_file}" DIRECTORY)
        add_subdirectory("${IOCP_PRACTICE_DIR}/${practice_directory}")
    endforeach()
endfunction()
