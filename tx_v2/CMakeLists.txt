cmake_minimum_required(VERSION 3.16)

# Componente partilhado com o RX: formato do pacote ALK.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

# Sem isto o IDF apanha a tag git mais proxima do repo como versao da app. Uma versao por etapa.
set(PROJECT_VER "audiolink-w2-tx-peer-audiocast")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(audiolink_tx_v2)
