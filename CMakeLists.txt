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
pkg_check_modules(CURL REQUIRED libcurl)
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

# --- UI module selection --------------------------------------------------
# The dockable catalog windows are General plugins that only make sense when
# the installed Audacious actually provides the matching interface plugin in
# its plugin directory. Each switch accepts:
#
#   AUTO - build when the interface plugin (qtui / gtkui) is shipped with the
#          installed Audacious AND the dev libraries are available (default)
#   ON   - force the module (hard error when the dev libraries are missing)
#   OFF  - skip the module
#
# Whatever the combination, the backend library and the input plugin are
# always built.
set(MSR_BUILD_QT_UI "AUTO" CACHE STRING
    "Qt catalog UI module: AUTO / ON / OFF")
set_property(CACHE MSR_BUILD_QT_UI PROPERTY STRINGS AUTO ON OFF)

set(MSR_BUILD_GTK_UI "AUTO" CACHE STRING
    "GTK3 catalog UI module: AUTO / ON / OFF")
set_property(CACHE MSR_BUILD_GTK_UI PROPERTY STRINGS AUTO ON OFF)

# Returns TRUE if <name>.so or <name>.dll ships in the Audacious plugin dir.
function(msr_interface_installed var name)
    set(_found FALSE)
    if(EXISTS "${AUDACIOUS_PLUGIN_DIR}/General/${name}.so")
        set(_found TRUE)
    elseif(EXISTS "${AUDACIOUS_PLUGIN_DIR}/General/${name}.dll")
        set(_found TRUE)
    endif()
    set(${var} "${_found}" PARENT_SCOPE)
endfunction()

# Resolves one tri-state switch (AUTO/ON/OFF) into a boolean.
function(msr_resolve_ui_switch option_var deps_ok interface_name resolved_var)
    set(_value "${${option_var}}")
    if(NOT _value STREQUAL "ON" AND NOT _value STREQUAL "OFF")
        set(_value "AUTO")
    endif()

    if(_value STREQUAL "ON")
        if(NOT deps_ok)
            message(FATAL_ERROR "${option_var}=ON requires missing build dependencies "
                "(interface '${interface_name}', see configure output above)")
        endif()
        set(_build TRUE)
        message(STATUS "Monster Siren: ${option_var}=ON - forcing the '${interface_name}' module")
    elseif(_value STREQUAL "OFF")
        set(_build FALSE)
        message(STATUS "Monster Siren: ${option_var}=OFF - skipping the '${interface_name}' module")
    else()
        msr_interface_installed(_iface "${interface_name}")
        if(_iface AND deps_ok)
            set(_build TRUE)
            message(STATUS "Monster Siren: ${option_var}=AUTO - '${interface_name}' detected, building")
        else()
            set(_build FALSE)
            if(NOT _iface)
                message(STATUS "Monster Siren: ${option_var}=AUTO - interface '${interface_name}' "
                    "not found in ${AUDACIOUS_PLUGIN_DIR}/General, skipping")
            else()
                message(STATUS "Monster Siren: ${option_var}=AUTO - deps missing, skipping")
            endif()
        endif()
    endif()

    set(${resolved_var} "${_build}" PARENT_SCOPE)
endfunction()

set(MSR_QT_DEPS_OK FALSE)
if(Qt6Widgets_FOUND OR TARGET Qt6::Widgets)
    set(MSR_QT_DEPS_OK TRUE)
endif()

msr_resolve_ui_switch(MSR_BUILD_QT_UI "${MSR_QT_DEPS_OK}" "qtui" MSR_BUILD_QT_UI_ENABLED)
msr_resolve_ui_switch(MSR_BUILD_GTK_UI "${GTK3_FOUND}" "gtkui" MSR_BUILD_GTK_UI_ENABLED)

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
    ${CURL_INCLUDE_DIRS}
)
target_link_libraries(msr_backend PUBLIC
    ${AUDACIOUS_LIBRARIES}
    ${CURL_LIBRARIES}
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
if(MSR_BUILD_QT_UI_ENABLED)
    message(STATUS "Monster Siren: enabling monster-siren-ui target")
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
    message(STATUS "Monster Siren: skipping monster-siren-ui target")
endif()

# 3. GTK General/UI Plugin Module (Dockable Catalog Window for -i gtk)
if(MSR_BUILD_GTK_UI_ENABLED)
    message(STATUS "Monster Siren: enabling monster-siren-gtk target (GTK ${GTK3_VERSION})")
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
    message(STATUS "Monster Siren: skipping monster-siren-gtk target")
endif()

message(STATUS "Audacious version: ${AUDACIOUS_VERSION}")
message(STATUS "Audacious plugin installation directory: ${AUDACIOUS_PLUGIN_DIR}")
