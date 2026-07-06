@echo off
REM Launch PASORAMA with verbose USB_PC.dll tracing enabled.
REM Useful for debugging: the replacement DLL writes a full per-call / per-transfer
REM trace to %TEMP%\usbpc_stub_log.txt (or USBPC_STUB_LOG if you set it).
REM Edit PASORAMA_EXE below if your install path differs.
setlocal
set USBPC_VERBOSE=1
set "PASORAMA_EXE=C:\Program Files\PASORAMA\df-x10000\PASORAMA.exe"
if not exist "%PASORAMA_EXE%" (
    echo ERROR: PASORAMA.exe not found at "%PASORAMA_EXE%". Edit this script.
    exit /b 1
)
echo USBPC_VERBOSE=%USBPC_VERBOSE%
echo Log: %TEMP%\usbpc_stub_log.txt
echo Starting PASORAMA... (keep this window open)
"%PASORAMA_EXE%"
