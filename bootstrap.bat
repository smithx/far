@echo off
if "%1" == "" (set arch=x64)
if "%1" == "X64" (set arch=x64)
if "%1" == "Win64" (set arch=x64)

if "%1" == "win32" (set arch=Win32)
if "%1" == "x86" (set arch=Win32)
if "%1" == "X86" (set arch=Win32)

set source_dir=%~dp0
set build_dir=%source_dir%\build-%arch%

cd %source_dir%
cmake -G "Visual Studio 16 2019" -A %arch% -S %source_dir% -B %build_dir% -DCMAKE_INSTALL_PREFIX="%source_dir%\Output"

echo Now you can either build the package
echo.
echo 	cmake --build build-%arch% --config Release
echo	cd build-%arch%
echo	cpack -G ZIP gitbranch
echo.
echo or open the solution file and start developing
echo.
echo 	build-%arch%\gitbranch.sln