#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

int create_socket();
int connect_socket(int sockfd, const char* ip, int port);
int bind_socket(int sockfd, int port);
int listen_socket(int sockfd);
int accept_connection(int sockfd);

#endif
