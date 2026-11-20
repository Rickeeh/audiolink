idf_component_register(SRCS "al_led.c" INCLUDE_DIRS "include" REQUIRES esp_driver_rmt)
# Codigo novo: avisos extra como erros (so neste componente).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
