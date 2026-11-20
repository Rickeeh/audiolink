cmake_minimum_required(VERSION 3.16)

# Sem isto, o IDF tenta "git describe" a partir da raiz do repo
# CumeStack e apanha a tag mais proxima de outro componente (ex.:
# sensor-node-01) como versao da app - confuso nos logs/OTA.
set(PROJECT_VER "audiolink-bench-v0.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(audiolink_bench)
