#include "sync.h"

int main(int argc, char* argv[]) {
    if (argc != 4) {
        printf("Uso: ./myClient <username> <server_ip_address> <port>\n");
        return 1;
    }

    const char* username = argv[1];
    const char* server_ip = argv[2];
    int port = atoi(argv[3]);

    sync_start(username, server_ip, port);
    return 0;
}
