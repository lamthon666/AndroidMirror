@echo off
chcp 65001 >nul
title Android Mirror Launcher
cd /d "C:\AndroidMirror\bin"

echo [1/4] Đang kiểm tra kết nối cáp USB với điện thoại...
adb.exe start-server
adb.exe wait-for-device

echo [2/4] Đang dọn dẹp phiên cũ và đẩy server.jar...
adb.exe shell pkill -f com.mirror.Server >nul 2>&1
adb.exe forward --remove-all >nul 2>&1
adb.exe push server.jar /data/local/tmp/server.jar >nul
adb.exe forward tcp:27183 localabstract:my_mirror_video
adb.exe forward tcp:27184 localabstract:my_mirror_audio

echo [3/4] Đang khởi chạy Server trên điện thoại...
start /B "" adb.exe shell CLASSPATH=/data/local/tmp/server.jar app_process / com.mirror.Server

:: Đợi 2 giây để Android Server khởi tạo xong Socket
timeout /t 2 /nobreak >nul

echo [4/4] Đang mở cửa sổ hiển thị...
start /WAIT "" mirror.exe

echo [*] Đang đóng kết nối...
adb.exe shell pkill -f com.mirror.Server >nul 2>&1
adb.exe forward --remove-all >nul 2>&1
exit