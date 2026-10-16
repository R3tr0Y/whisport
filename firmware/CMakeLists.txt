# whisport 固件 —— 板级底座复用官方 FoloToy AI-Passport BSP（components/bsp）
# 前置：官方仓库克隆到 vendor/ai-passport（见 firmware/README.md）
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../vendor/ai-passport/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(whisport)
