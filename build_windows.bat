@echo off
REM Build script for VpeNativeInput on Windows

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
cmake .. -G "Visual Studio 17 2022" -A x64
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
echo Output: VisualPinball.Engine\VisualPinball.Unity\VisualPinball.Unity\Plugins\win-x64\VpeNativeInput.dll
echo.
echo Next steps:
echo 1. Open Unity project
echo 2. Add SimulationThreadComponent to your table GameObject
echo 3. Enable "Enable Native Input" in Inspector
echo 4. Press Play
echo.
