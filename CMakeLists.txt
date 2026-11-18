cmake_minimum_required(VERSION 3.25)
project(litehtml VERSION 0.10.0 LANGUAGES C CXX)
include(GNUInstallDirs)

option(LITEHTML_BUILD_STATIC "Build the static library" ON)
option(LITEHTML_BUILD_SHARED "Build the shared library" ON)
file(GLOB gumbo_sources CONFIGURE_DEPENDS src/gumbo/src/*.c)
add_library(gumbo STATIC ${gumbo_sources})
target_include_directories(gumbo PUBLIC src/gumbo/src)
set_target_properties(gumbo PROPERTIES POSITION_INDEPENDENT_CODE ON)
file(GLOB_RECURSE html_sources CONFIGURE_DEPENDS src/*.cpp)

function(add_litehtml target type)
    add_library(${target} ${type} ${html_sources})
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/litehtml>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    target_link_libraries(${target} PRIVATE gumbo)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()
if(LITEHTML_BUILD_STATIC)
    add_litehtml(litehtml STATIC)
    if(WIN32)
        set_target_properties(litehtml PROPERTIES OUTPUT_NAME litehtml_static)
    endif()
endif()
if(LITEHTML_BUILD_SHARED)
    add_litehtml(litehtml_shared SHARED)
    set_target_properties(litehtml_shared PROPERTIES OUTPUT_NAME litehtml
        VERSION ${PROJECT_VERSION} SOVERSION 0 WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if(WIN32 AND TARGET litehtml_shared)
    enable_language(RC)
    configure_file(cmake/LibraryVersion.rc.in LibraryVersion.rc @ONLY)
    target_sources(litehtml_shared PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/LibraryVersion.rc")
endif()
