@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"
pushd "%REPO_ROOT%"
if errorlevel 1 exit /b %errorlevel%

if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
    set "PLATFORM=ARM64"
    set "VSARCH=arm64"
    set "LOCAL_BUILDDIR=altair_local\build-msvc-arm64"
    set "MCP_BUILDDIR=altair_mcp_server\build-msvc-arm64"
) else (
    set "PLATFORM=x64"
    set "VSARCH=amd64"
    set "LOCAL_BUILDDIR=altair_local\build-msvc"
    set "MCP_BUILDDIR=altair_mcp_server\build-msvc"
)

set "VSDEVCMD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%~I\Common7\Tools\VsDevCmd.bat" (
            set "VSDEVCMD=%%~I\Common7\Tools\VsDevCmd.bat"
        )
    )

    if not defined VSDEVCMD (
        for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath`) do (
            if exist "%%~I\Common7\Tools\VsDevCmd.bat" (
                set "VSDEVCMD=%%~I\Common7\Tools\VsDevCmd.bat"
            )
        )
    )

    if not defined VSDEVCMD (
        for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do (
            if exist "%%~I\Common7\Tools\VsDevCmd.bat" (
                set "VSDEVCMD=%%~I\Common7\Tools\VsDevCmd.bat"
            )
        )
    )
)

if not defined VSDEVCMD (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" (
        set "VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
    )
)

if not defined VSDEVCMD (
    echo Could not locate VsDevCmd.bat. Install Visual Studio 2022 Build Tools with Desktop development with C++.
    popd
    exit /b 1
)

call "%VSDEVCMD%" -arch=%VSARCH%
if errorlevel 1 (
    popd
    exit /b %errorlevel%
)

cmake -S altair_local -B "%LOCAL_BUILDDIR%" -G "Visual Studio 17 2022" -A %PLATFORM%
if errorlevel 1 (
    popd
    exit /b %errorlevel%
)

cmake --build "%LOCAL_BUILDDIR%" --config Release
if errorlevel 1 (
    popd
    exit /b %errorlevel%
)

cmake -S altair_mcp_server -B "%MCP_BUILDDIR%" -G "Visual Studio 17 2022" -A %PLATFORM%
if errorlevel 1 (
    popd
    exit /b %errorlevel%
)

cmake --build "%MCP_BUILDDIR%" --config Release
if errorlevel 1 (
    popd
    exit /b %errorlevel%
)

echo Windows host tools built successfully.
popd
exit /b 0