// AudioLink - PCM estereo de 24 bit empacotado: o payload do protocolo v3. Puro (sem IDF); testado no host.
//
// De onde vem (TX):
//   O I2S le o PCM1808 em slots de 32 bit (Philips, 64 BCK por trama). O driver entrega cada amostra como um int32 com os
//   24 bits do ADC alinhados a esquerda: bits 31..8, complemento para dois, com o sinal no bit 31. O byte baixo nao tem
//   dados do ADC (o adc_test.c ja faz >> 8). E exatamente o formato do payload v2.
//
// No fio (v3):
//   - por amostra, 3 bytes little-endian com os bits 31..8 da palavra:
//       byte 0 = bits 8..15   (byte menos significativo da amostra de 24 bit)
//       byte 1 = bits 16..23
//       byte 2 = bits 24..31  (o mais significativo, com o sinal)
//   - ordem das amostras L, R, L, R, ..., trama a trama, como no v2: 128 tramas = 256 amostras = 768 B;
//   - valor representado: inteiro de 24 bit com sinal, de -8 388 608 (0x800000) a +8 388 607 (0x7FFFFF).
//
// De volta a int32 (RX):
//   - cada amostra volta a int32 alinhado a esquerda, (b0 << 8) | (b1 << 16) | (b2 << 24), com o byte baixo a 0;
//   - o sinal fica no bit 31 por construcao. O pipeline do RX trabalha em int32 alinhado a esquerda (o que o I2S do DAC
//     consome), por isso nao precisa de estender o sinal;
//   - para o valor de 24 bit alinhado a direita usa-se alk_pcm24_s24(): extensao de sinal sem depender do shift aritmetico
//     de valores negativos, que o C deixa a implementacao.
//
// Ida e volta: unpack(pack(w)) == (w & 0xFFFFFF00) para todo o int32 w. Bit-exato nos 24 bits do ADC.
#pragma once

#include <stddef.h>
#include <stdint.h>

// TX: n amostras int32 alinhadas a esquerda -> 3n bytes. Le os bits 31..8 de cada amostra; ignora o byte baixo.
void alk_pcm24_pack(const int32_t *src, size_t n, uint8_t *dst);

// RX: 3n bytes -> n amostras int32 alinhadas a esquerda (byte baixo 0), escritas em dst como 4n bytes little-endian.
// dst nao precisa de estar alinhado.
void alk_pcm24_unpack(const uint8_t *src, size_t n, uint8_t *dst_le32);

// Valor de 24 bit com sinal, alinhado a direita, de uma amostra int32 alinhada a esquerda.
static inline int32_t alk_pcm24_s24(int32_t left_aligned) {
    const uint32_t v = ((uint32_t)left_aligned >> 8) & 0xFFFFFFu;
    return (int32_t)(v ^ 0x800000u) - 0x800000;
}

// RX: copia um pacote ALK ja validado (magic, versao 2 ou 3, 1 <= frames <= ALK_FRAMES_PER_PKT, comprimento) para dst no
// layout v2 (cabecalho + int32 por amostra). Devolve 28 + frames x 8. dst tem de ter ALK_V2_PKT_BYTES livres.
// O cabecalho e copiado tal como chegou, com a versao incluida. Devolve 0, sem escrever nada, se o pacote violar as
// pre-condicoes (defesa; o RX ja valida antes).
size_t alk_to_v2_layout(const uint8_t *pkt, uint8_t *dst);
