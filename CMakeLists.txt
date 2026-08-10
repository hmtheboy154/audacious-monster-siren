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

# Find Qt6 Core (required for QJsonDocument and QString in backend/input)
find_package(Qt6 REQUIRED COMPONENTS Core)

# Find optional Qt6 Widgets (required for Qt UI plugin)
find_package(Qt6 QUIET COMPONENTS Widgets)

# Check for GTK3
pkg_check_modules(GTK3 gtk+-3.0)

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
    Qt6::Core
)
set_target_properties(msr_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 1. Input Plugin Module (Handles msr:// and Monster Siren URLs, expanding albums & playing streams)
add_library(monster-siren-input MODULE src/msr_input.cpp)
target_include_directories(monster-siren-input PRIVATE ${AUDACIOUS_INCLUDE_DIRS})
target_link_libraries(monster-siren-input PRIVATE
    msr_backend
    ${AUDACIOUS_LIBRARIES}
    Qt6::Core
)
set_target_properties(monster-siren-input PROPERTIES
    PREFIX ""
    OUTPUT_NAME "monster-siren-input"
)
install(TARGETS monster-siren-input
    LIBRARY DESTINATION "${AUDACIOUS_PLUGIN_DIR}/Input"
)

# 2. General/UI Plugin Module (Dockable Catalog Window & Settings for -i qt)
if(Qt6Widgets_FOUND OR TARGET Qt6::Widgets)
    message(STATUS "Qt6Widgets found, enabling monster-siren-ui target")
    add_library(monster-siren-ui MODULE src/msr_ui.cpp)
    target_include_directories(monster-siren-ui PRIVATE ${AUDACIOUS_INCLUDE_DIRS})
    target_link_libraries(monster-siren-ui PRIVATE
        msr_backend
        ${AUDACIOUS_LIBRARIES}
        Qt6::Core
        Qt6::Widgets
    )
    set_target_properties(monster-siren-ui PROPERTIES
        PREFIX ""
        OUTPUT_NAME "monster-siren-ui"
    )
    install(TARGETS monster-siren-ui
        LIBRARY DESTINATION "${AUDACIOUS_PLUGIN_DIR}/General"
    )
else()
    message(STATUS "Qt6Widgets not found, skipping monster-siren-ui target")
endif()

# 3. GTK General/UI Plugin Module (Dockable Catalog Window for -i gtk)
if(GTK3_FOUND)
    message(STATUS "GTK3 found (${GTK3_VERSION}), enabling monster-siren-gtk target")
    add_library(monster-siren-gtk MODULE src/msr_ui_gtk.cpp)
    target_include_directories(monster-siren-gtk PRIVATE ${AUDACIOUS_INCLUDE_DIRS} ${GTK3_INCLUDE_DIRS})
    target_link_libraries(monster-siren-gtk PRIVATE
        msr_backend
        ${AUDACIOUS_LIBRARIES}
        ${GTK3_LIBRARIES}
    )
    set_target_properties(monster-siren-gtk PROPERTIES
        PREFIX ""
        OUTPUT_NAME "monster-siren-gtk"
    )
    install(TARGETS monster-siren-gtk
        LIBRARY DESTINATION "${AUDACIOUS_PLUGIN_DIR}/General"
    )
else()
    message(STATUS "GTK3 not found, skipping monster-siren-gtk target")
endif()

message(STATUS "Audacious version: ${AUDACIOUS_VERSION}")
message(STATUS "Audacious plugin installation directory: ${AUDACIOUS_PLUGIN_DIR}")
