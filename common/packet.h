#ifndef PACKET_H
#define PACKET_H

#include <cstdint>

typedef struct packet {
    uint16_t type;          // Tipo do pacote (DATA | CMD)
    uint16_t seqn;          // Número de sequência
    uint32_t total_size;    // Número total de fragmentos
    uint16_t length;        // Comprimento do payload
    char payload[1024];     // Dados do pacote
} packet;

#endif
