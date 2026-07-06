@echo off
REM Build the WinUSB replacement USB_PC.dll (32-bit) for PASORAMA on Windows 10/11.
REM Requires Visual Studio Build Tools (2022 or newer) with the x86 C toolchain,
REM and the Windows SDK (for winusb.lib / setupapi.lib).
REM
REM Output: build\USB_PC.dll  (drop this into your PASORAMA install directory).
setlocal
set "ROOT=%~dp0"

REM --- locate vcvarsall.bat (adjust if your VS is installed elsewhere) ---
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    echo ERROR: vcvarsall.bat not found. Install Visual Studio Build Tools ^(x86 C++^) and edit this path.
    exit /b 1
)

REM PASORAMA.exe is 32-bit, so USB_PC.dll must be x86.
call "%VCVARS%" x86
if errorlevel 1 exit /b 1

if not exist "%ROOT%build" mkdir "%ROOT%build"
pushd "%ROOT%build"
del /q USB_PC.dll USB_PC.lib USB_PC.exp usb_pc_stub.obj 2>nul

cl.exe /nologo /W4 /WX /TC /LD "%ROOT%src\usb_pc_stub.c" ^
    /link /nologo /def:"%ROOT%src\usb_pc_stub.def" /out:USB_PC.dll /implib:USB_PC.lib ^
    winusb.lib setupapi.lib
set "BUILD_RC=%ERRORLEVEL%"
if "%BUILD_RC%"=="0" dumpbin /exports USB_PC.dll
popd

if "%BUILD_RC%"=="0" (
    echo.
    echo Built: %ROOT%build\USB_PC.dll
)
exit /b %BUILD_RC%
