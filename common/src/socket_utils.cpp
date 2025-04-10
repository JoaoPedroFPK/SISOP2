#include "../headers/socket_utils.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>  // For TCP_NODELAY
#include <unistd.h>
#include <cstring>
#include <errno.h>
#include <sys/select.h>
#include <stdio.h>

int create_socket() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    
    // Configure socket for better transmission
    if (sockfd >= 0) {
        // Disable Nagle's algorithm to send small packets immediately
        int flag = 1;
        if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
            printf("WARNING: Failed to set TCP_NODELAY: %s\n", strerror(errno));
        }
        
        // Set larger send/receive buffers
        int bufsize = 64 * 1024; // 64KB
        setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    }
    
    return sockfd;
}

int connect_socket(int sockfd, const char* ip, int port) {
    sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &serv_addr.sin_addr);
    return connect(sockfd, (sockaddr*)&serv_addr, sizeof(serv_addr));
}

int bind_socket(int sockfd, int port) {
    sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
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

// Helper functions for reliable packet transmission
size_t write_all(int sockfd, const void* buf, size_t len) {
    size_t total_written = 0;
    while (total_written < len) {
        ssize_t written = write(sockfd, 
                               (const char*)buf + total_written, 
                               len - total_written);
        if (written <= 0) {
            if (errno == EINTR) continue; // Interrupted, try again
            return total_written;         // Error or connection closed
        }
        total_written += written;
    }
    return total_written;
}

size_t read_all(int sockfd, void* buf, size_t len) {
    size_t total_read = 0;
    // Add timeout to prevent infinite blocking
    struct timeval tv;
    fd_set readfds;
    
    while (total_read < len) {
        // Set up select timeout (1 second)
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        // Wait for socket to be readable
        int select_result = select(sockfd + 1, &readfds, NULL, NULL, &tv);
        if (select_result <= 0) {
            if (select_result == 0) {
                printf("DEBUG Socket: Read timeout after reading %zu bytes\n", total_read);
            } else {
                printf("DEBUG Socket: Select error: %s\n", strerror(errno));
            }
            break; // Timeout or error
        }
        
        // Socket is readable, attempt read
        ssize_t bytes_read = read(sockfd, 
                                (char*)buf + total_read, 
                                len - total_read);
        if (bytes_read <= 0) {
            if (errno == EINTR) continue; // Interrupted, try again
            
            // Error or connection closed
            printf("DEBUG Socket: Read error: %s\n", strerror(errno));
            break;
        }
        
        total_read += bytes_read;
        printf("DEBUG Socket: Read progress: %zu/%zu bytes\n", total_read, len);
    }
    
    return total_read;
}
