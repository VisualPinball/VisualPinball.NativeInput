cmake_minimum_required(VERSION 3.15)
project(VpeNativeInput VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

# Source files
set(SOURCES
    src/NativeInput.h
)

# Platform-specific sources
if(WIN32)
    list(APPEND SOURCES src/NativeInput_Windows.cpp)
    set(PLATFORM_LIBS winmm)
elseif(APPLE)
    list(APPEND SOURCES src/NativeInput_Mac.mm)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(APPLICATION_SERVICES_FRAMEWORK ApplicationServices REQUIRED)
    set(PLATFORM_LIBS ${APPKIT_FRAMEWORK} ${APPLICATION_SERVICES_FRAMEWORK})
elseif(UNIX)
    list(APPEND SOURCES src/NativeInput_Linux.cpp)
    find_package(Threads REQUIRED)
    find_package(X11 REQUIRED)
    set(PLATFORM_LIBS Threads::Threads ${X11_LIBRARIES})
else()
    list(APPEND SOURCES src/NativeInput_Stub.cpp)
    set(PLATFORM_LIBS)
endif()

# Create shared library
add_library(VpeNativeInput SHARED ${SOURCES})

# Link platform libraries
target_link_libraries(VpeNativeInput PRIVATE ${PLATFORM_LIBS})

if(UNIX AND NOT APPLE)
    target_include_directories(VpeNativeInput PRIVATE ${X11_INCLUDE_DIR})
endif()

# Set output directories based on platform
set(VPE_NATIVEINPUT_OUTPUT_DIR "" CACHE PATH "Override output directory for built native binaries")

if(VPE_NATIVEINPUT_OUTPUT_DIR)
    set(OUTPUT_DIR "${VPE_NATIVEINPUT_OUTPUT_DIR}")
elseif(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../VisualPinball.Engine/VisualPinball.Unity/VisualPinball.Unity/Plugins/win-x64")
    else()
        set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../VisualPinball.Engine/VisualPinball.Unity/VisualPinball.Unity/Plugins/win-x86")
    endif()
elseif(APPLE)
    set(VPE_OSX_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    if(CMAKE_OSX_ARCHITECTURES)
        list(GET CMAKE_OSX_ARCHITECTURES 0 VPE_OSX_ARCH)
    endif()

    if(VPE_OSX_ARCH MATCHES "^(arm64|aarch64)$")
        set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../VisualPinball.Engine/VisualPinball.Unity/VisualPinball.Unity/Plugins/osx/arm64")
    else()
        set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../VisualPinball.Engine/VisualPinball.Unity/VisualPinball.Unity/Plugins/osx/x64")
    endif()
elseif(UNIX)
    set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../VisualPinball.Engine/VisualPinball.Unity/VisualPinball.Unity/Plugins/linux-x64")
endif()

# Set output directory
set_target_properties(VpeNativeInput PROPERTIES
    OUTPUT_NAME "VisualPinball.NativeInput"
    LIBRARY_OUTPUT_DIRECTORY ${OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR}
    ARCHIVE_OUTPUT_DIRECTORY ${OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_DIR}
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${OUTPUT_DIR}
)

# Install rules
install(TARGETS VpeNativeInput
    LIBRARY DESTINATION ${OUTPUT_DIR}
    RUNTIME DESTINATION ${OUTPUT_DIR}
)
