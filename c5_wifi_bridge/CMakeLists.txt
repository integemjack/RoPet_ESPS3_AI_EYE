# C5 WiFi Bridge - 顶层工程 CMake
# 目标芯片: esp32c5 (支持 2.4G/5G 双频 WiFi6)
# 用法:
#   idf.py set-target esp32c5
#   idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c5_wifi_bridge)
