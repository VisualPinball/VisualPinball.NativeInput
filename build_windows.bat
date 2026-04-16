@echo off
REM Build script for VpeNativeInput on Windows

set SCRIPT_DIR=%~dp0
set OUTPUT_DIR=%SCRIPT_DIR%artifacts\win-x64

echo ========================================
echo Building VpeNativeInput for Windows
echo ========================================

REM Check if CMake is installed
where cmake >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: CMake not found. Please install CMake and add it to PATH.
    exit /b 1
)

REM Create build directory
if not exist build mkdir build
cd build

REM Configure CMake (Visual Studio 2022, x64)
echo.
echo Configuring CMake...
cmake .. -G "Visual Studio 17 2022" -A x64 -DNATIVEINPUT_OUTPUT_DIR="%OUTPUT_DIR%"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: CMake configuration failed.
    cd ..
    exit /b 1
)

REM Build Release configuration
echo.
echo Building Release configuration...
cmake --build . --config Release
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Build failed.
    cd ..
    exit /b 1
)

cd ..

echo.
echo ========================================
echo Build complete!
echo ========================================
echo.
echo Output: artifacts\win-x64\VisualPinball.NativeInput.dll
echo.
echo Next steps:
echo 1. Pack/publish VisualPinball.NativeInput NuGet package from artifacts
echo 2. Update VisualPinball.Engine package version if needed
echo 3. Build VisualPinball.Engine to copy binaries into Unity Plugins
echo.
