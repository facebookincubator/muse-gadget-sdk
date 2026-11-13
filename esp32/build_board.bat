@echo off
call D:\esp\esp-idf-v6.0.3\export.bat
cd /d d:\muse-gadget-sdk\esp32
idf.py -B build -DIDF_TARGET=esp32s3 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-lcdwiki-s3-28;devices/sdkconfig.muse-realtime-voice" build

