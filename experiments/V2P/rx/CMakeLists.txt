cmake_minimum_required(VERSION 3.16)
# AudioLink V2 - prototipo do RX (docs/AUDIOLINK_V2_PROTOTYPE_PLAN_2026-11-01.md). Nao e producao.
set(PROJECT_VER "v2p-rx")
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(v2p_rx)
