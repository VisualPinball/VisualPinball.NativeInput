cmake_minimum_required(VERSION 3.15)
project(VpeNativeInput VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(SOURCES
    src/NativeInput.h
)

# Platform-specific sources
if(WIN32)
    list(APPEND SOURCES src/NativeInput_Windows.cpp)
    set(PLATFORM_LIBS winmm)
else()
    list(APPEND SOURCES src/NativeInput_Stub.cpp)
    set(PLATFORM_LIBS)
endif()

# Create shared library
add_library(VpeNativeInput SHARED ${SOURCES})

# Link platform libraries
target_link_libraries(VpeNativeInput PRIVATE ${PLATFORM_LIBS})

# Set output directories based on platform
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../VisualPinball.Engine/VisualPinball.Unity/VisualPinball.Unity/Plugins/win-x64")
    else()
        set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../VisualPinball.Engine/VisualPinball.Unity/VisualPinball.Unity/Plugins/win-x86")
    endif()
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
        set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../VisualPinball.Engine/VisualPinball.Unity/VisualPinball.Unity/Plugins/osx/arm64")
    else()
        set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../VisualPinball.Engine/VisualPinball.Unity/VisualPinball.Unity/Plugins/osx/x64")
    endif()
elseif(UNIX)
    set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../VisualPinball.Engine/VisualPinball.Unity/VisualPinball.Unity/Plugins/linux-x64")
endif()

# Set output directory
set_target_properties(VpeNativeInput PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR}
)

# Install rules
install(TARGETS VpeNativeInput
    LIBRARY DESTINATION ${OUTPUT_DIR}
    RUNTIME DESTINATION ${OUTPUT_DIR}
)
