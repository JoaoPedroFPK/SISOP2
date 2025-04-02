#include "connection_handler.h"

int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Uso: ./server <porta>\n");
        return 1;
    }

    int port = atoi(argv[1]);
    run_server(port);
    return 0;
}
