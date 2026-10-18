@echo off
chcp 65001 >nul
echo [1/2] Đang biên dịch lại Android Server (server.jar)...
cd /d "C:\Users\LENOVO\AndroidStudioProjects\MirrorServer"
call gradlew.bat exportServerJar
copy /Y "build\server\server.jar" "C:\AndroidMirror\bin\server.jar"

echo [2/2] Đang biên dịch lại Windows Client (mirror.exe)...
C:\msys64\ucrt64\bin\g++.exe -O3 "C:\AndroidMirror\src\client\main.cpp" -o "C:\AndroidMirror\bin\mirror.exe" -lavcodec -lavutil -lSDL2 -lws2_32 -mwindows

echo [HOÀN TẤT] Đã cập nhật bản build mới nhất vào C:\AndroidMirror\bin!
pause