#include "sync.h"
#include "../commons/socket_utils.h"
#include <cstdio>

void sync_start(const char* username, const char* server_ip, int port) {
    printf("Iniciando sessão para o usuário %s...\n", username);
    int sockfd = create_socket();
    if (connect_socket(sockfd, server_ip, port) == 0) {
        printf("Conectado ao servidor %s:%d\n", server_ip, port);
        // Lógica de sincronização será implementada aqui.
    } else {
        perror("Erro ao conectar");
    }
}
