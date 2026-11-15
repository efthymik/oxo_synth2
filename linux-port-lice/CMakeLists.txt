cmake_minimum_required(VERSION 3.25)

set(OXO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(OXO_SDK "${OXO_ROOT}/third_party/vst3sdk")

if(NOT EXISTS "${OXO_SDK}/public.sdk/source/vst/vstaudioeffect.cpp")
    message(FATAL_ERROR "Pinned Steinberg SDK missing at third_party/vst3sdk (git submodule update --init --recursive)")
endif()


# Parse exactly the same identity as the original macOS CMake project.
foreach(OXO_IDENTITY_FILE Identity.xcconfig Identity.local.xcconfig)
    set(OXO_IDENTITY_PATH "${OXO_ROOT}/${OXO_IDENTITY_FILE}")
    if(EXISTS "${OXO_IDENTITY_PATH}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${OXO_IDENTITY_PATH}")
        file(STRINGS "${OXO_IDENTITY_PATH}" OXO_IDENTITY_LINES REGEX "^[A-Z_0-9]+[ \t]*=")
        foreach(OXO_LINE IN LISTS OXO_IDENTITY_LINES)
            string(REGEX MATCH "^([A-Z_0-9]+)[ \t]*=[ \t]*(.*)$" OXO_UNUSED "${OXO_LINE}")
            string(STRIP "${CMAKE_MATCH_2}" "${CMAKE_MATCH_1}")
        endforeach()
    endif()
endforeach()
foreach(OXO_KEY OXO_PRODUCT_NAME OXO_VENDOR OXO_VENDOR_WEBSITE OXO_VENDOR_EMAIL OXO_BUNDLE_ID
        OXO_VST3_PROCESSOR_UID OXO_VST3_CONTROLLER_UID OXO_DATA_FOLDER OXO_LIBRARY_FOLDER
        MARKETING_VERSION CURRENT_PROJECT_VERSION)
    if("${${OXO_KEY}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${OXO_KEY} in Identity.xcconfig")
    endif()
endforeach()
# Keep the Steinberg SDK project version tied to the original Synth2 identity.
project(Synth2LinuxVST3 VERSION "${MARKETING_VERSION}" LANGUAGES C CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Linux x64 is required.")
endif()
set(OXO_MARKETING_VERSION "${MARKETING_VERSION}")
set(OXO_BUILD_NUMBER "${CURRENT_PROJECT_VERSION}")
string(REPLACE " " "" OXO_VST3_PACKAGE_NAME "${OXO_PRODUCT_NAME}")
foreach(OXO_UID PROCESSOR CONTROLLER)
    string(REGEX REPLACE "([0-9A-Fa-f]+)" "0x\\1," OXO_TEXT "${OXO_VST3_${OXO_UID}_UID}")
    string(REGEX REPLACE ",$" "" OXO_VST3_${OXO_UID}_UID_WORDS "${OXO_TEXT}")
endforeach()
set(OXO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${OXO_GENERATED_DIR}")
configure_file("${OXO_ROOT}/OxoSynth2VST3/source/OxoSynth2Identity.h.in"
    "${OXO_GENERATED_DIR}/OxoSynth2Identity.h" @ONLY)

# Avoid all SDK link installation/symlink attempts (no Admin/Developer Mode).
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
# The SDK's editorhost sample is NOT built (it demands GTK3 and its run loop
# spins / starves timers); linux-port-lice/HostTest is our fixed fork instead:
# -DOXO_HOST_TEST=ON, then target oxo_editorhost.
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_AUV2_BUILDS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${OXO_SDK}" "${CMAKE_CURRENT_BINARY_DIR}/vst3sdk")
smtg_enable_vst3_sdk()

# The SDK is a pinned submodule we never edit: its own warnings (deprecated
# std::wstring_convert, int64 format specifiers) are not ours to fix, so
# compile its targets with warnings off. Our sources keep -Wsuggest-override.
foreach(OXO_SDK_TARGET pluginterfaces base sdk_common sdk_hosting sdk moduleinfotool validator)
    if(TARGET ${OXO_SDK_TARGET})
        target_compile_options(${OXO_SDK_TARGET} PRIVATE -w)
    endif()
endforeach()

# Dependencies: libxcb (the window), libxcb-keysyms (key symbols), the system
# font engine (FreeType + fontconfig, as JUCE / VSTGUI / DPF use it) and zlib
# (the zip bank reader; FreeType loads libz.so.1 anyway, so a vendored copy
# would only add a second zlib to the process).
find_package(PkgConfig REQUIRED)
pkg_check_modules(OXO_XCB REQUIRED IMPORTED_TARGET xcb xcb-keysyms)
pkg_check_modules(OXO_TEXTDEPS REQUIRED IMPORTED_TARGET freetype2 fontconfig)
# zlib through pkg-config rather than FindZLIB: FindZLIB caches an absolute
# ZLIB_LIBRARY, which is wrong on multiarch systems (/usr/lib/libz.so does not
# exist there, only /usr/lib/<triplet>/libz.so) and stale in an old build dir.
pkg_check_modules(OXO_ZLIBDEPS REQUIRED IMPORTED_TARGET zlib)

# LICE (Cockos WDL, zlib/libpng): the drawing engine, compiled in. Only the
# core rasterizer files; no image codecs, no GL, no SWELL (a few stubs stand
# in for the SWELL calls in its unused LICE_SysBitmap).
add_library(oxo_lice STATIC
    "${CMAKE_CURRENT_LIST_DIR}/Dependencies/wdl/lice/lice.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Dependencies/wdl/lice/lice_line.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Dependencies/wdl/lice/lice_arc.cpp"
)
target_compile_features(oxo_lice PRIVATE cxx_std_17)
# Vendored third-party code, never edited here: compile it with warnings off,
# and make its headers SYSTEM so their warnings (the SDK's -Wsuggest-override
# on lice.h) stay out of every one of our translation units that includes them.
target_include_directories(oxo_lice SYSTEM PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/Dependencies/wdl"
    "${CMAKE_CURRENT_LIST_DIR}/Dependencies/wdl/lice")
target_compile_options(oxo_lice PRIVATE -w)

set(OXO_SRC "${OXO_ROOT}/OxoSynth2VST3/source")
smtg_add_vst3plugin(OxoSynth2VST3
    PACKAGE_NAME "${OXO_VST3_PACKAGE_NAME}"
    SOURCES_LIST
        "${OXO_SRC}/OxoSynth2CIDs.h"
        "${OXO_SRC}/OxoSynth2Processor.cpp"
        "${OXO_SRC}/OxoSynth2Processor.h"
        "${OXO_SRC}/OxoSynth2Controller.cpp"
        "${OXO_SRC}/OxoSynth2Controller.h"
        "${OXO_SRC}/OxoSynth2Entry.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/LinuxEditor.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/LinuxEditor.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/LinuxPanel.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/LinuxPanel.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/LinuxX11.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/LinuxX11.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/LinuxCanvas.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/LinuxCanvas.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/LinuxText.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/LinuxText.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/LiceSwellStubs.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/LinuxTrash.cpp"
        "${OXO_ROOT}/EditorCore/State.hpp"
        "${OXO_ROOT}/EditorCore/Storage.hpp"
        "${OXO_ROOT}/EditorCore/SynthEditorCore.hpp"
)
target_compile_features(OxoSynth2VST3 PRIVATE cxx_std_20)
# The shared controller (OxoSynth2VST3/source/OxoSynth2Controller.cpp) builds
# this panel when OXO_LINUX_EDITOR_LICE is defined; Linux_Build/ leaves it
# undefined and builds DesktopEditor instead.
target_compile_definitions(OxoSynth2VST3 PRIVATE OXO_LINUX_EDITOR_LICE)
# SYSTEM for the SDK root: its headers define stricmp/strnicmp differently from
# WDL's swell-types.h, and that redefinition is third-party noise, not ours.
target_include_directories(OxoSynth2VST3 SYSTEM PRIVATE "${OXO_SDK}")
target_include_directories(OxoSynth2VST3 PRIVATE
    "${OXO_ROOT}" "${OXO_GENERATED_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/Source")
# No -ffast-math: the engine's DSP must keep IEEE semantics.
target_link_libraries(OxoSynth2VST3 PRIVATE sdk oxo_lice PkgConfig::OXO_XCB PkgConfig::OXO_TEXTDEPS PkgConfig::OXO_ZLIBDEPS)
# A shared library may carry undefined symbols by default, which then only
# show up as a load failure in the host: fail the build instead.
#
# The version script exports the C entry points and hides everything else, so
# the plugin's C++ symbols cannot interpose on the host or on another plugin.
# That is also what makes the static C++ runtime safe: our libstdc++ symbols
# stay local, so the two runtimes in the process never meet.
target_link_options(OxoSynth2VST3 PRIVATE
    "LINKER:--no-undefined"
    "LINKER:--version-script=${CMAKE_CURRENT_LIST_DIR}/vst3.map"
    "LINKER:--exclude-libs,ALL")
# The C++ runtime is static: the export surface is C-only (vst3.map) and our
# symbols stay local, so the two runtimes in the process never meet. This also
# removes the GLIBCXX version-skew failure mode.
target_link_options(OxoSynth2VST3 PRIVATE -static-libgcc -static-libstdc++)

# Match the original plugin's bundled factory and starter banks (unaltered).
file(GLOB OXO_FACTORY_PATCHES CONFIGURE_DEPENDS "${OXO_ROOT}/Shared/Resources/Synth2 Factory/*.sy1")
smtg_target_add_plugin_resources(OxoSynth2VST3 RESOURCES ${OXO_FACTORY_PATCHES}
    OUTPUT_SUBDIRECTORY "Synth2 Factory")
file(GLOB OXO_STARTER_BANKS LIST_DIRECTORIES true CONFIGURE_DEPENDS
    "${OXO_ROOT}/Shared/Resources/Synth2 Starter Banks/*")
foreach(OXO_BANK IN LISTS OXO_STARTER_BANKS)
    if(IS_DIRECTORY "${OXO_BANK}")
        get_filename_component(OXO_BANK_NAME "${OXO_BANK}" NAME)
        file(GLOB OXO_BANK_PATCHES CONFIGURE_DEPENDS "${OXO_BANK}/*.sy1")
        smtg_target_add_plugin_resources(OxoSynth2VST3 RESOURCES ${OXO_BANK_PATCHES}
            OUTPUT_SUBDIRECTORY "Synth2 Starter Banks/${OXO_BANK_NAME}")
    endif()
endforeach()

# The SDK builds its own `validator` target (the same 47 checks run on mac /
# Windows) under build/vst3sdk/.../validator — build_linux.sh runs it on the
# bundle it produces.

# The controlled test host (-DOXO_HOST_TEST=ON, target oxo_editorhost):
# linux-port-lice/HostTest, an X11/XEmbed host that pumps the editor through the
# host's Linux::IRunLoop the way a DAW does. Test tooling; never shipped.
if(OXO_HOST_TEST)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/HostTest" "${CMAKE_CURRENT_BINARY_DIR}/HostTest")
endif()
