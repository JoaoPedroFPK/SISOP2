#include "socket_utils.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

int create_socket() {
    return socket(AF_INET, SOCK_STREAM, 0);
}

int connect_socket(int sockfd, const char* ip, int port) {
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &serv_addr.sin_addr);
    return connect(sockfd, (sockaddr*)&serv_addr, sizeof(serv_addr));
}

int bind_socket(int sockfd, int port) {
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    return bind(sockfd, (sockaddr*)&serv_addr, sizeof(serv_addr));
}

int listen_socket(int sockfd) {
    return listen(sockfd, 10);
}

int accept_connection(int sockfd) {
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    return accept(sockfd, (sockaddr*)&client_addr, &client_len);
}
