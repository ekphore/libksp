# SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20)
project(libksp VERSION 0.1.0 LANGUAGES CXX)

# Headers are co-located with implementation; export the same namespaced SDK
# layout for build-tree and installed consumers.
set(LIBKSP_PUBLIC_HEADERS
    src/ksp.h
)
foreach(header IN LISTS LIBKSP_PUBLIC_HEADERS)
    get_filename_component(name "${header}" NAME)
    configure_file("${header}" "${CMAKE_CURRENT_BINARY_DIR}/public/libksp/${name}" COPYONLY)
endforeach()

include(GNUInstallDirs)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/libksp)

add_library(libksp
    src/builtins.cpp
    src/ksp.cpp
    src/lexer.cpp
    src/parser.cpp
    src/semantic.cpp)
add_library(libksp::libksp ALIAS libksp)
target_compile_features(libksp PUBLIC cxx_std_20)
target_include_directories(libksp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/public>
    $<INSTALL_INTERFACE:include>)
if(MSVC)
    target_compile_options(libksp PRIVATE /W4 /permissive-)
else()
    target_compile_options(libksp PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS libksp EXPORT libkspTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${LIBKSP_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libksp)
install(EXPORT libkspTargets FILE libkspTargets.cmake NAMESPACE libksp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libksp)
