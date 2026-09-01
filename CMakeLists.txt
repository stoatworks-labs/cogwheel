cmake_minimum_required(VERSION 3.15)
project(Cogwheel VERSION 0.1.0 LANGUAGES C CXX)

#-----------------------------------------------------------------------------
# Architecture. Must sit immediately after project() and before any target
# exists: CMake latches the architecture list when the first target is created,
# and setting it later is silently ignored -- the build succeeds and produces a
# single-architecture binary. Verify the result with `lipo`, never with the log.
#-----------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

# Apple deprecated the whole OpenGL API in 10.14 and Resolume's plugin interface
# is still OpenGL, so every SDK call warns. Silencing it is not ignoring a
# problem -- there is no alternative API to move to here.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()
if(MSVC)
    add_compile_options(/utf-8)
endif()
if(WIN32)
    # windows.h defines min/max as macros, which turns every std::min into
    # `std::(...)` and fails with an error naming neither min nor windows.h.
    add_compile_definitions(NOMINMAX)
endif()

option(COGWHEEL_BUILD_TOOLS "Build the offline render and measurement harness" ON)
option(BUILD_OFX            "Build the OpenFX plugin" ON)

#-----------------------------------------------------------------------------
# The FFGL SDK
#-----------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

#-----------------------------------------------------------------------------
# The shared core.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor and nothing ever references that object by name, so in
# an archive the linker is entitled to drop the whole translation unit -- and
# the result is a bundle that loads, exports plugMain, and reports that it
# contains no plugins.
#
# Note what is NOT in here: SourcePlugin.cpp and EffectPlugin.cpp. Putting
# either registration in the shared library would register both plugins into
# both bundles.
#-----------------------------------------------------------------------------
add_library(cogwheel_core OBJECT
    source/Cogwheel.h               source/Cogwheel.cpp
    source/Controls.h               source/Controls.cpp
    source/Presets.h
    source/Config.h                 source/Config.cpp
    source/Diag.h                   source/Diag.cpp
    source/StoatworksAbout.h        source/StoatworksAboutParams.h
    source/StoatworksAboutLinks.h

    source/machine/Step.h
    source/machine/Clock.h          source/machine/Clock.cpp
    source/machine/Gears.h          source/machine/Gears.cpp
    source/machine/Wheels.h         source/machine/Wheels.cpp
    source/machine/Crank.h          source/machine/Crank.cpp
    source/machine/Pens.h           source/machine/Pens.cpp

    source/render/GLState.h
    source/render/PaperBuffer.h     source/render/PaperBuffer.cpp
    source/render/Shaders.h         source/render/Shaders.cpp
    source/render/Sheet.h           source/render/Sheet.cpp
)

target_include_directories(cogwheel_core PUBLIC source)
target_link_libraries(cogwheel_core PUBLIC ffgl::sdk)
target_compile_definitions(cogwheel_core PUBLIC COGWHEEL_VERSION="${PROJECT_VERSION}")
set_target_properties(cogwheel_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The SDK headers pull <GL/glew.h> on Windows and Linux, and the SDK links GLEW
# privately -- so consumers need the include path and the GLEW_STATIC define the
# static vcpkg triplet propagates. macOS uses the system OpenGL framework.
#
# GLEW arrives through `vcpkg.json`, which nothing in this file points at. Leave
# that manifest out of a new repo and every local build and every macOS CI job
# passes while the Windows job fails at CONFIGURE, here.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(cogwheel_core PUBLIC GLEW::GLEW)
endif()

#-----------------------------------------------------------------------------
# The two plugins.
#
# FFGL resolves one plugMain per binary, so a source and an effect are two
# bundles -- not one bundle with two entries.
#-----------------------------------------------------------------------------
function(cogwheel_add_plugin target entry bundle_name identifier)
    add_library(${target} MODULE ${entry})
    target_link_libraries(${target} PRIVATE cogwheel_core)

    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "${bundle_name}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${identifier}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
        target_link_libraries(${target} PRIVATE
            "-framework OpenGL" "-framework Carbon" "-framework AppKit")
    else()
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${bundle_name}")
    endif()
endfunction()

cogwheel_add_plugin(CogwheelSource source/SourcePlugin.cpp
    "Cogwheel" "com.stoatworks.ffgl.cogwheel")
cogwheel_add_plugin(CogwheelEffect source/EffectPlugin.cpp
    "Cogwheel Ink" "com.stoatworks.ffgl.cogwheel.ink")

#-----------------------------------------------------------------------------
# The offline harness. macOS only, one translation unit, and it links the same
# OBJECT library the bundles do -- so it measures the code that ships.
#-----------------------------------------------------------------------------
if(COGWHEEL_BUILD_TOOLS AND APPLE)
    add_executable(cgtest tools/cgtest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines in the
    # harness rather than a vendored dependency.
    target_link_libraries(cgtest PRIVATE cogwheel_core z
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()

#-----------------------------------------------------------------------------
# Install: straight into Resolume's plugin folder.
#-----------------------------------------------------------------------------
if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()

install(TARGETS CogwheelSource CogwheelEffect
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)

#-----------------------------------------------------------------------------
# OpenFX. Not in the install() list -- it goes to /Library/OFX/Plugins by hand
# and ships as its own zip.
#-----------------------------------------------------------------------------
if(BUILD_OFX)
    set(OFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/external/openfx")

    # NOTE: `cogwheel_core` is LINKED below, not spliced in as
    # `$<TARGET_OBJECTS:cogwheel_core>`.
    #
    # TARGET_OBJECTS pulls in the compiled objects and nothing else -- no include
    # directories, no compile definitions, none of the usage requirements the
    # library declares. So this target compiles happily on macOS, where the SDK
    # headers reach the system OpenGL framework, and fails on Windows with
    # "Cannot open include file: 'GL/glew.h'" from inside FFGL.h, because GLEW
    # arrives through cogwheel_core's PUBLIC link to GLEW::GLEW and that link is
    # exactly what TARGET_OBJECTS throws away.
    add_library(CogwheelOFX MODULE
        source/ofx/CogwheelOFX.cpp
        ${OFX_SDK}/Support/Library/ofxsCore.cpp
        ${OFX_SDK}/Support/Library/ofxsImageEffect.cpp
        ${OFX_SDK}/Support/Library/ofxsInteract.cpp
        ${OFX_SDK}/Support/Library/ofxsLog.cpp
        ${OFX_SDK}/Support/Library/ofxsMultiThread.cpp
        ${OFX_SDK}/Support/Library/ofxsParams.cpp
        ${OFX_SDK}/Support/Library/ofxsProperty.cpp
        ${OFX_SDK}/Support/Library/ofxsPropertyValidation.cpp
    )
    target_include_directories(CogwheelOFX PRIVATE
        source
        "${OFX_SDK}/include" "${OFX_SDK}/Support/include")
    target_link_libraries(CogwheelOFX PRIVATE cogwheel_core)
    target_compile_definitions(CogwheelOFX PRIVATE
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR})
    if(APPLE)
        target_link_libraries(CogwheelOFX PRIVATE "-framework OpenGL" "-framework AppKit")
    endif()

    # Threads, explicitly. The OFX Support library's multi-thread suite calls
    # pthread_create, and on Rocky 8 -- glibc 2.28, the floor Resolve supports --
    # that still lives in libpthread; libpthread was only folded into libc in
    # glibc 2.34. Leave it out and the plugin compiles, links, passes a symbol
    # check and a glibc-version check, and then fails at dlopen with "undefined
    # symbol: pthread_create". macOS and Windows never show it.
    find_package(Threads REQUIRED)
    target_link_libraries(CogwheelOFX PRIVATE Threads::Threads)

    # libGL, explicitly, on Linux. Cogwheel's OFX build links GL at all because
    # the shared core reaches <FFGLSDK.h> through the renderer. When GLEW is
    # linked dynamically this dependency is invisible: libGLEW.so carries its
    # own NEEDED on libGL and the loader follows it. Link GLEW statically and
    # nothing pulls libGL in any more -- the plugin then builds, links, and
    # fails at dlopen with "undefined symbol: glEnable".
    if(UNIX AND NOT APPLE)
        # LEGACY, deliberately. Left to itself CMake prefers GLVND, and
        # OpenGL::GL then resolves to libOpenGL.so.0 + libGLX.so.0 -- which ship
        # in libglvnd-*, NOT in mesa-libGL. The plugin builds fine and fails at
        # dlopen with "libOpenGL.so.0: cannot open shared object file".
        set(OpenGL_GL_PREFERENCE LEGACY)
        find_package(OpenGL REQUIRED)
        target_link_libraries(CogwheelOFX PRIVATE OpenGL::GL)
    endif()

    set(OFX_BUNDLE_DIR "${CMAKE_BINARY_DIR}/Cogwheel.ofx.bundle")
    set_target_properties(CogwheelOFX PROPERTIES
        PREFIX "" SUFFIX ".ofx" OUTPUT_NAME "Cogwheel"
        LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/MacOS")

    if(APPLE)
        set(OFX_BUNDLE_IDENTIFIER "com.stoatworks.cogwheel.ofx")
        configure_file(cmake/InfoOFX.plist.in
            "${OFX_BUNDLE_DIR}/Contents/Info.plist" @ONLY)
    elseif(WIN32)
        set_target_properties(CogwheelOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            # MSVC's .exp/.lib collides with the FFGL target's, which shares the
            # output base name; a parallel build then races with LNK1104.
            ARCHIVE_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/importlib"
            ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/importlib")
    else()
        set_target_properties(CogwheelOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Linux-x86-64")
    endif()
endif()
