@echo off
REM Downloads or prepares the FidelityFX FSR 3 native DX12 backend for VIctory.
REM Analogous to Get-NVIDIA-DLSS.cmd.
REM
REM After running, set:
REM   set VICTORY_FSR3_DIR=fsr3-sdk
REM   set VICTORY_FSR3_FG=1
REM before launching (especially useful on RTX 30-series / 3080 Ti).
REM
echo.
echo === FSR 3 SDK setup for VIctory (RTX 30-series Frame Generation) ===
echo.
echo 1. Clone or download the FidelityFX SDK FSR3 branch:
echo    https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
echo    (use the fsr3-v3.x branch)
echo.
echo 2. Build the native DX12 backend (BuildFSRSolutionNative.bat or the CMake path).
echo.
echo 3. Copy the resulting DLLs / libs into a folder (e.g. fsr3-sdk\ next to this script).
echo.
echo 4. Set VICTORY_FSR3_DIR to that folder and VICTORY_FSR3_FG=1.
echo.
echo Current default lookup path is C:\dev\sdk\FidelityFX-SDK-FSR3
echo.
echo See docs/FSR3_INTEGRATION.md for the full design and remaining TODOs.
echo.
pause
