cmake_minimum_required(VERSION 3.16)
project(KillSwitch LANGUAGES C)

# Only Windows executables carry an icon and manifest inside the file itself.
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(KillSwitch
    main.c
    include/app.c
    include/config.c
    include/hidctl.c
    include/inputblock.c
    include/ui.c
    include/usbdev.c
    include/proto.c
    include/tray.c
    include/window.c
    include/instance.c
    include/console.c
    include/respath.c
    include/watchdog.c
    libs/cJSON.c
)

# Clay is a single-header library; exactly one translation unit defines its
# implementation, and ui.c is that unit.
set_source_files_properties(include/ui.c PROPERTIES
    COMPILE_DEFINITIONS "CLAY_IMPLEMENTATION")
set_target_properties(KillSwitch PROPERTIES OUTPUT_NAME "KillSwitch")

if(WIN32)
    set_target_properties(KillSwitch PROPERTIES SUFFIX ".exe")
    # GUI subsystem: no console window behind the interface. console.c opens
    # one on demand when config.json sets "debug". MinGW keeps main() under
    # -mwindows.
    set_target_properties(KillSwitch PROPERTIES WIN32_EXECUTABLE TRUE)
    # Icon for Explorer plus the manifest that asks for administrator rights.
    target_sources(KillSwitch PRIVATE resources/icon.rc)
endif()

target_include_directories(KillSwitch PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs")

# --- raylib, the renderer behind Clay ----------------------------------------
find_package(raylib CONFIG QUIET)
if(TARGET raylib)
    target_link_libraries(KillSwitch PRIVATE raylib)
    message(STATUS "raylib found via: CMake package")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RAYLIB QUIET IMPORTED_TARGET raylib)
    endif()
    if(TARGET PkgConfig::RAYLIB)
        target_link_libraries(KillSwitch PRIVATE PkgConfig::RAYLIB)
        message(STATUS "raylib found via: pkg-config")
    else()
        message(FATAL_ERROR
            "raylib not found.\n"
            "  vcpkg:         vcpkg install raylib:<triplet>\n"
            "  Debian/Ubuntu: sudo apt install libraylib-dev\n"
            "  macOS:         brew install raylib")
    endif()
endif()

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(KillSwitch PRIVATE m Threads::Threads)
endif()

# --- platform shell: tray, window icon, and HID device switching -------------
if(WIN32)
    # shell32/user32/gdi32/ole32: the tray and window icon.
    # setupapi/cfgmgr32: enabling and disabling the HID device nodes.
    target_link_libraries(KillSwitch PRIVATE
        shell32 user32 gdi32 ole32 setupapi cfgmgr32)
else()
    # The kill switch itself is Windows-only (hidctl.c is a stub elsewhere), but
    # the interface, USB link and tray still build so the project is portable.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(APPINDICATOR QUIET IMPORTED_TARGET ayatana-appindicator3-0.1)
    endif()
    if(TARGET PkgConfig::APPINDICATOR)
        target_link_libraries(KillSwitch PRIVATE PkgConfig::APPINDICATOR)
        target_compile_definitions(KillSwitch PRIVATE TRAY_HAVE_APPINDICATOR)
        message(STATUS "tray: Ayatana AppIndicator")
    else()
        message(WARNING "libayatana-appindicator not found: no tray icon.")
    endif()
endif()

# The interface loads its fonts from resources/ beside the executable.
add_custom_command(TARGET KillSwitch POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/resources"
            "$<TARGET_FILE_DIR:KillSwitch>/resources")

if(MSVC)
    target_compile_definitions(KillSwitch PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(KillSwitch PRIVATE /W3)
else()
    target_compile_options(KillSwitch PRIVATE -Wall -Wextra)
endif()

# --- locate libusb-1.0 -------------------------------------------------------
#
# Resolution order:
#   1. pkg-config   Linux, macOS, MSYS2, and vcpkg when pkgconf is on PATH
#   2. find_path / find_library   vcpkg, Homebrew, system, or -DLIBUSB_ROOT=<dir>
#
set(LIBUSB_SOURCE "")
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
endif()

if(TARGET PkgConfig::LIBUSB)
    target_link_libraries(KillSwitch PRIVATE PkgConfig::LIBUSB)
    set(LIBUSB_SOURCE "pkg-config")
else()
    find_path(LIBUSB_INCLUDE_DIR
        NAMES libusb.h
        PATH_SUFFIXES libusb-1.0
        HINTS ${LIBUSB_ROOT} ENV LIBUSB_ROOT)
    find_library(LIBUSB_LIBRARY
        NAMES usb-1.0 libusb-1.0
        HINTS ${LIBUSB_ROOT} ENV LIBUSB_ROOT)

    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
        set(_hint "")
        if(DEFINED VCPKG_TARGET_TRIPLET)
            set(_hint
                "\n  This build uses vcpkg triplet '${VCPKG_TARGET_TRIPLET}'."
                "\n  Install libusb for it: vcpkg install libusb:${VCPKG_TARGET_TRIPLET}")
        endif()
        message(FATAL_ERROR
            "libusb-1.0 not found.\n"
            "  vcpkg:         vcpkg install libusb:<triplet>\n"
            "  Debian/Ubuntu: sudo apt install libusb-1.0-0-dev\n"
            "  macOS:         brew install libusb\n"
            "  MSYS2:         pacman -S mingw-w64-x86_64-libusb\n"
            "  unpacked:      -DLIBUSB_ROOT=C:/path/to/libusb"
            ${_hint})
    endif()

    target_include_directories(KillSwitch PRIVATE ${LIBUSB_INCLUDE_DIR})
    target_link_libraries(KillSwitch PRIVATE ${LIBUSB_LIBRARY})
    set(LIBUSB_SOURCE "${LIBUSB_LIBRARY}")

    if(WIN32)
        # libusb reaches WinUSB through the setup API and COM.
        target_link_libraries(KillSwitch PRIVATE setupapi ole32 advapi32)
    endif()
endif()

# Put the libusb runtime DLL beside the executable for a MinGW build.
if(WIN32 AND LIBUSB_LIBRARY)
    get_filename_component(_libusb_libdir "${LIBUSB_LIBRARY}" DIRECTORY)
    get_filename_component(_libusb_prefix "${_libusb_libdir}" DIRECTORY)
    find_file(LIBUSB_RUNTIME_DLL
        NAMES libusb-1.0.dll usb-1.0.dll
        HINTS "${_libusb_prefix}/bin" "${_libusb_libdir}"
        NO_DEFAULT_PATH)
    if(LIBUSB_RUNTIME_DLL)
        add_custom_command(TARGET KillSwitch POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${LIBUSB_RUNTIME_DLL}" "$<TARGET_FILE_DIR:KillSwitch>")
    endif()
endif()

message(STATUS "libusb-1.0 found via: ${LIBUSB_SOURCE}")
