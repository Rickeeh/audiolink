// TX v2 - tarefa de tempo real (tx_audio): aquisicao -> pacote ALK -> esp_now_send, um bloco de 128 tramas por vez.
#pragma once

void tx_audio_task(void *arg);
