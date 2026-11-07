cmake_minimum_required(VERSION 3.16)
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
    set(PLATFORM_LIBS winmm hid setupapi cfgmgr32 user32)
elseif(APPLE)
    list(APPEND SOURCES src/NativeInput_Mac.mm src/OpenPinballInput.cpp src/OpenPinballMac.cpp)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(APPLICATION_SERVICES_FRAMEWORK ApplicationServices REQUIRED)
    find_library(CORE_FOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
    set(PLATFORM_LIBS ${APPKIT_FRAMEWORK} ${APPLICATION_SERVICES_FRAMEWORK} ${CORE_FOUNDATION_FRAMEWORK} ${IOKIT_FRAMEWORK})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SOURCES src/NativeInput_Linux.cpp src/OpenPinballInput.cpp src/OpenPinballLinux.cpp)
    find_package(Threads REQUIRED)
    find_package(X11 REQUIRED)
    set(PLATFORM_LIBS Threads::Threads ${X11_LIBRARIES})
else()
    list(APPEND SOURCES src/NativeInput_Stub.cpp)
    set(PLATFORM_LIBS)
endif()

if(WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "" FORCE)
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
    foreach(subsystem AUDIO VIDEO HAPTIC POWER SENSOR DIALOG TRAY)
        set(SDL_${subsystem} OFF CACHE BOOL "" FORCE)
    endforeach()
    if(APPLE)
        # SDL's macOS HIDAPI driver runs its own report thread. The IOKit and
        # GameController joystick drivers can be sampled by our polling thread.
        set(SDL_HIDAPI_JOYSTICK OFF CACHE BOOL "" FORCE)
    endif()
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
        URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f)
    FetchContent_MakeAvailable(SDL3)
    set_target_properties(SDL3-static PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
    target_compile_definitions(SDL3-static PRIVATE SDL_DECLSPEC=)
    list(APPEND SOURCES src/ControllerInput.cpp src/NativeButtons.cpp)
    list(APPEND PLATFORM_LIBS SDL3::SDL3-static)
endif()

# Create shared library
add_library(VpeNativeInput SHARED ${SOURCES})

if(WIN32)
    target_compile_definitions(VpeNativeInput PRIVATE WINVER=0x0600 _WIN32_WINNT=0x0600)
endif()

# Link platform libraries
target_link_libraries(VpeNativeInput PRIVATE ${PLATFORM_LIBS})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_include_directories(VpeNativeInput PRIVATE ${X11_INCLUDE_DIR})
endif()

# Set output directories based on platform
# Defaults to artifacts/<rid> so this project stays independent of VPE.
set(NATIVEINPUT_OUTPUT_DIR "" CACHE PATH "Override output directory for built native binaries")
# Backward-compatible alias for old scripts/workflows.
set(VPE_NATIVEINPUT_OUTPUT_DIR "" CACHE PATH "Deprecated alias for NATIVEINPUT_OUTPUT_DIR")

set(NATIVEINPUT_RID "")

if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(NATIVEINPUT_RID "win-x64")
    else()
        set(NATIVEINPUT_RID "win-x86")
    endif()
elseif(APPLE)
    set(VPE_OSX_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    if(CMAKE_OSX_ARCHITECTURES)
        list(GET CMAKE_OSX_ARCHITECTURES 0 VPE_OSX_ARCH)
    endif()

    if(VPE_OSX_ARCH MATCHES "^(arm64|aarch64)$")
        set(NATIVEINPUT_RID "osx-arm64")
    else()
        set(NATIVEINPUT_RID "osx-x64")
    endif()
elseif(UNIX)
    set(NATIVEINPUT_RID "linux-x64")
endif()

if(NATIVEINPUT_OUTPUT_DIR)
    set(OUTPUT_DIR "${NATIVEINPUT_OUTPUT_DIR}")
elseif(VPE_NATIVEINPUT_OUTPUT_DIR)
    set(OUTPUT_DIR "${VPE_NATIVEINPUT_OUTPUT_DIR}")
elseif(NATIVEINPUT_RID)
    set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/artifacts/${NATIVEINPUT_RID}")
else()
    set(OUTPUT_DIR "${CMAKE_BINARY_DIR}/artifacts")
endif()

message(STATUS "NativeInput output directory: ${OUTPUT_DIR}")

# Set output directory
set_target_properties(VpeNativeInput PROPERTIES
    OUTPUT_NAME "VisualPinball.NativeInput"
    CXX_VISIBILITY_PRESET hidden
    OBJCXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
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

option(NATIVEINPUT_BUILD_TESTS "Build native input report tests" OFF)
if(NATIVEINPUT_BUILD_TESTS)
    enable_testing()
    add_executable(OpenPinballDeviceTests tests/OpenPinballDeviceTests.cpp src/OpenPinballInput.cpp src/NativeButtons.cpp)
    target_include_directories(OpenPinballDeviceTests PRIVATE src)
    add_test(NAME OpenPinballDeviceTests COMMAND OpenPinballDeviceTests)
    if(TARGET SDL3::SDL3-static)
        add_executable(ControllerInputTests tests/ControllerInputTests.cpp src/ControllerInput.cpp src/NativeButtons.cpp)
        target_include_directories(ControllerInputTests PRIVATE src)
        target_link_libraries(ControllerInputTests PRIVATE SDL3::SDL3-static)
        add_test(NAME ControllerInputTests COMMAND ControllerInputTests)
    endif()
endif()
