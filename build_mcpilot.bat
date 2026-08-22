@echo off
set IDF_TOOLS_PATH=C:\Espressif
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.12_env
set ADF_PATH=D:\ai_work\esp-adf
cd /d C:\Espressif\frameworks\esp-idf-v5.5.5
call export.bat
if errorlevel 1 (
    echo [ERROR] export.bat failed
    exit /b 1
)
cd /d D:\ai_work\xiaozhi-esp32-music-player
echo === build ===
idf.py build
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo === BUILD SUCCESS ===
