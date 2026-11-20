cmake_minimum_required(VERSION 3.16)

# Sem isto o IDF apanha a tag git mais proxima do repo CumeStack como versao da app.
set(PROJECT_VER "audiolink-audio-v0.1")

# Formato ALK partilhado com o TX v2 (protocolo v3, PCM24): so o RX o usa (link_rx.c).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(audiolink_audio)
