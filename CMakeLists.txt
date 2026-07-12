cmake_minimum_required(VERSION 3.16)

project(audacious-monster-siren
    VERSION 1.0.0
    DESCRIPTION "Audacious plugin to fetch and play albums & songs from Monster Siren (Arknights OST)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)

# Find PkgConfig and dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(AUDACIOUS REQUIRED audacious>=4.6.1)
pkg_get_variable(AUDACIOUS_PLUGIN_DIR audacious plugin_dir)

if(NOT AUDACIOUS_PLUGIN_DIR)
    set(AUDACIOUS_PLUGIN_DIR "${CMAKE_INSTALL_PREFIX}/lib/audacious")
endif()

# Find Qt6 (Core, Network, Widgets)
find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)

# Common include directories and compiler flags
add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_definitions(
    PACKAGE="audacious-monster-siren"
    HAVE_GETTEXT=1
)

# Backend static helper library
add_library(msr_backend STATIC
    src/monster_siren.cpp
    src/monster_siren.h
)
target_include_directories(msr_backend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${AUDACIOUS_INCLUDE_DIRS}
)
target_link_libraries(msr_backend PUBLIC
    ${AUDACIOUS_LIBRARIES}
    audqt
    Qt6::Core
    Qt6::Network
)
set_target_properties(msr_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 1. Input Plugin Module (Handles msr:// and Monster Siren URLs, expanding albums & playing streams)
add_library(monster-siren-input MODULE src/msr_input.cpp)
target_include_directories(monster-siren-input PRIVATE ${AUDACIOUS_INCLUDE_DIRS})
target_link_libraries(monster-siren-input PRIVATE
    msr_backend
    ${AUDACIOUS_LIBRARIES}
    audqt
    Qt6::Core
    Qt6::Network
)
set_target_properties(monster-siren-input PROPERTIES
    PREFIX ""
    OUTPUT_NAME "monster-siren-input"
)

# 2. General/UI Plugin Module (Dockable Catalog Window & Settings)
add_library(monster-siren-ui MODULE src/msr_ui.cpp)
target_include_directories(monster-siren-ui PRIVATE ${AUDACIOUS_INCLUDE_DIRS})
target_link_libraries(monster-siren-ui PRIVATE
    msr_backend
    ${AUDACIOUS_LIBRARIES}
    audqt
    Qt6::Core
    Qt6::Network
    Qt6::Widgets
)
set_target_properties(monster-siren-ui PROPERTIES
    PREFIX ""
    OUTPUT_NAME "monster-siren-ui"
)

# Installation targets
install(TARGETS monster-siren-input
    LIBRARY DESTINATION "${AUDACIOUS_PLUGIN_DIR}/Input"
)
install(TARGETS monster-siren-ui
    LIBRARY DESTINATION "${AUDACIOUS_PLUGIN_DIR}/General"
)

message(STATUS "Audacious version: ${AUDACIOUS_VERSION}")
message(STATUS "Audacious plugin installation directory: ${AUDACIOUS_PLUGIN_DIR}")
