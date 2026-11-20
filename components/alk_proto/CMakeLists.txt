idf_component_register(SRCS "alk_pcm24.c" INCLUDE_DIRS "include")
# Codigo novo: avisos extra como erros (so neste componente).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
