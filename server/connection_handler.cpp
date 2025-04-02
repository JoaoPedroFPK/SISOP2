#include "connection_handler.h"
#include "../commons/socket_utils.h"
#include <pthread.h>
#include <cstdio>
#include <cstdlib>

void* handle_client(void* client_sockfd);

void run_server(int port) {
    int sockfd = create_socket();
    bind_socket(sockfd, port);
    listen_socket(sockfd);

    printf("Servidor rodando na porta %d...\n", port);

    while (true) {
        int client_sockfd = accept_connection(sockfd);
        pthread_t thread_id;
        pthread_create(&thread_id, NULL, handle_client, (void*)(intptr_t)client_sockfd);
    }
}

void* handle_client(void* arg) {
    int sockfd = (intptr_t)arg;
    printf("Cliente conectado!\n");
    // Implementar comunicação com o cliente aqui.
    close(sockfd);
    pthread_exit(NULL);
}
