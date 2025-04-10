#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

#include <cstddef> // For size_t

int create_socket();
int connect_socket(int sockfd, const char* ip, int port);
int bind_socket(int sockfd, int port);
int listen_socket(int sockfd);
int accept_connection(int sockfd);

// Reliable I/O functions
size_t write_all(int sockfd, const void* buf, size_t len);
size_t read_all(int sockfd, void* buf, size_t len);

#endif
