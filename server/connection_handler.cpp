#include "connection_handler.h"
#include "file_manager.h"
#include "../common/packet.h"
#include "../common/socket_utils.h"
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <sys/socket.h>
#include <netinet/tcp.h>  // For TCP_NODELAY and IPPROTO_TCP
#include <fcntl.h>

// Global FileManager instance
static FileManager fileManager;

// Mutex for protecting concurrent access to shared resources
static pthread_mutex_t fileMutex = PTHREAD_MUTEX_INITIALIZER;

// Keep track of connected clients
struct ClientInfo {
    std::string username;
    int sockfd;
};

static std::unordered_map<std::string, std::vector<ClientInfo>> connectedClients;
static pthread_mutex_t clientsMutex = PTHREAD_MUTEX_INITIALIZER;

// Enum for packet types
enum PacketType {
    CMD_LOGIN = 1,
    CMD_UPLOAD = 2,
    CMD_DOWNLOAD = 3,
    CMD_DELETE = 4,
    CMD_LIST_SERVER = 5,
    CMD_LIST_CLIENT = 6,
    CMD_GET_SYNC_DIR = 7,
    DATA_PACKET = 8,
    SYNC_NOTIFICATION = 9,
    CMD_EXIT = 10
};

void* handle_client(void* client_sockfd);
void register_client(const std::string& username, int sockfd);
void unregister_client(const std::string& username, int sockfd);
void notify_clients(const std::string& username, const packet& pkt, int excludeSockfd);
void process_command(int sockfd, packet& pkt);

void run_server(int port) {
    int sockfd = create_socket();
    bind_socket(sockfd, port);
    listen_socket(sockfd);

    printf("Servidor rodando na porta %d...\n", port);

    while (true) {
        int client_sockfd = accept_connection(sockfd);
        pthread_t thread_id;
        pthread_create(&thread_id, NULL, handle_client, (void*)(intptr_t)client_sockfd);
        pthread_detach(thread_id);
    }
}

void* handle_client(void* arg) {
    int sockfd = (intptr_t)arg;
    printf("Cliente conectado!\n");
    
    // Configure the socket for proper data reception
    struct timeval timeout;
    timeout.tv_sec = 30;  // Increase from 5 to 30 seconds
    timeout.tv_usec = 0;
    
    // Set socket timeouts
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("Error setting receive timeout on server");
    }
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("Error setting send timeout on server");
    }
    
    // Enable keep-alive
    int keepalive = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
        perror("Error setting SO_KEEPALIVE on server");
    }
    
    // Set socket to blocking mode
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK);
    
    // Set TCP_NODELAY to disable Nagle's algorithm
    int flag = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        perror("Error setting TCP_NODELAY on server");
    }
    
    packet pkt;
    memset(&pkt, 0, sizeof(packet)); // Clear the packet before reading
    std::string username;
    
    // Read login packet using standard read to avoid potential issues with custom read_all
    printf("DEBUG Server: Waiting to read login packet (size=%zu bytes)...\n", sizeof(packet));
    ssize_t direct_bytes = recv(sockfd, &pkt, sizeof(packet), MSG_WAITALL);
    printf("DEBUG Server: Direct read returned %zd bytes\n", direct_bytes);
    
    if (direct_bytes > 0) {
        printf("DEBUG Server: Received packet header - type: %d, seqn: %d, length: %d\n",
               pkt.type, pkt.seqn, pkt.length);
        
        // Validate payload length
        if (pkt.length > sizeof(pkt.payload)) {
            printf("ERROR: Invalid payload length: %d (max: %zu)\n", pkt.length, sizeof(pkt.payload));
            close(sockfd);
            pthread_exit(NULL);
        }
        
        // Continue processing even with partial read as long as we have the essential header
        if (pkt.type == CMD_LOGIN) {
            // Ensure the payload is null-terminated
            pkt.payload[pkt.length < sizeof(pkt.payload) ? pkt.length : sizeof(pkt.payload) - 1] = '\0';
            username = pkt.payload;
            printf("Login de usuário: %s (seq: %d)\n", username.c_str(), pkt.seqn);
            
            // Register client
            register_client(username, sockfd);
            
            // Initialize user directory
            pthread_mutex_lock(&fileMutex);
            fileManager.initUserDirectory(username);
            pthread_mutex_unlock(&fileMutex);
            
            // Send login confirmation with SAME sequence number
            packet response;
            memset(&response, 0, sizeof(packet)); // Clear response packet
            response.type = CMD_LOGIN;
            response.seqn = pkt.seqn;  // Use client's sequence number
            response.total_size = 0;
            response.length = 0;
            
            printf("DEBUG Server: Sending login response with seq: %d\n", response.seqn);
            ssize_t bytes_sent = send(sockfd, &response, sizeof(packet), 0);
            printf("DEBUG Server: Direct send returned %zd bytes\n", bytes_sent);
            
            // Process commands
            while (true) {
                memset(&pkt, 0, sizeof(packet)); // Clear packet before reading
                
                // Use direct socket operations to read command packets
                printf("DEBUG Server: Waiting for next command packet...\n");
                errno = 0; // Clear errno before the call
                ssize_t cmd_bytes = recv(sockfd, &pkt, sizeof(packet), MSG_WAITALL);
                
                if (cmd_bytes <= 0) {
                    printf("DEBUG Server: Failed to read command packet: %s (errno=%d)\n", 
                           strerror(errno), errno);
                    
                    // Check if connection is still alive
                    int error = 0;
                    socklen_t len = sizeof(error);
                    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
                        printf("DEBUG Server: Socket is in error state, closing connection.\n");
                        break;
                    }
                    
                    // If it's a timeout, we can try again
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        printf("DEBUG Server: Timeout waiting for command, retrying...\n");
                        continue;
                    }
                    
                    break;
                }
                
                if (cmd_bytes < sizeof(packet)) {
                    printf("DEBUG Server: Read incomplete packet (%zd of %zu bytes)\n", 
                           cmd_bytes, sizeof(packet));
                    break;
                }
                
                printf("DEBUG Server: Received command packet type: %d, seq: %d\n", pkt.type, pkt.seqn);
                process_command(sockfd, pkt);
                
                if (pkt.type == CMD_EXIT) {
                    break;
                }
            }
        } else {
            printf("ERROR: Expected login packet (type 1), but received type %d\n", pkt.type);
        }
    } else {
        printf("ERROR: Failed to read login packet: %s\n", strerror(errno));
    }
    
    // Unregister client on disconnect
    if (!username.empty()) {
        unregister_client(username, sockfd);
    }
    
    close(sockfd);
    printf("Cliente desconectado: %s\n", username.c_str());
    pthread_exit(NULL);
}

void register_client(const std::string& username, int sockfd) {
    pthread_mutex_lock(&clientsMutex);
    
    // Add to connected clients
    ClientInfo info;
    info.username = username;
    info.sockfd = sockfd;
    connectedClients[username].push_back(info);
    
    pthread_mutex_unlock(&clientsMutex);
}

void unregister_client(const std::string& username, int sockfd) {
    pthread_mutex_lock(&clientsMutex);
    
    // Remove from connected clients
    auto& clients = connectedClients[username];
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        if (it->sockfd == sockfd) {
            clients.erase(it);
            break;
        }
    }
    
    if (clients.empty()) {
        connectedClients.erase(username);
    }
    
    pthread_mutex_unlock(&clientsMutex);
}

void notify_clients(const std::string& username, const packet& pkt, int excludeSockfd) {
    pthread_mutex_lock(&clientsMutex);
    
    // Send notification to all connected clients of this user except the source
    auto it = connectedClients.find(username);
    if (it != connectedClients.end()) {
        for (const auto& client : it->second) {
            if (client.sockfd != excludeSockfd) {
                printf("DEBUG Server: Notifying client on socket %d about file: %s\n", 
                       client.sockfd, pkt.payload);
                send(client.sockfd, &pkt, sizeof(packet), 0);
            }
        }
    }
    
    pthread_mutex_unlock(&clientsMutex);
}

void process_command(int sockfd, packet& pkt) {
    packet response;
    memset(&response, 0, sizeof(packet)); // Clear the response packet
    response.type = pkt.type;
    response.seqn = pkt.seqn;  // Important: Use the same sequence number from the request
    
    std::string username;
    // Get username from connected clients
    pthread_mutex_lock(&clientsMutex);
    for (const auto& entry : connectedClients) {
        for (const auto& client : entry.second) {
            if (client.sockfd == sockfd) {
                username = client.username;
                break;
            }
        }
        if (!username.empty()) break;
    }
    pthread_mutex_unlock(&clientsMutex);
    
    if (username.empty()) {
        return;
    }
    
    printf("DEBUG Server: Processing command type %d from user: %s, seq: %d\n", 
           pkt.type, username.c_str(), pkt.seqn);
    
    switch (pkt.type) {
        case CMD_UPLOAD: {
            // Extract filename from the payload
            std::string filename(pkt.payload);
            printf("DEBUG Server: Received upload command for file: %s, size: %u bytes\n", 
                   filename.c_str(), pkt.total_size);
            
            // Receive file data
            char* fileData = new char[pkt.total_size];
            size_t bytesRead = 0;
            
            // Loop to receive all file data packets
            while (bytesRead < pkt.total_size) {
                packet dataPkt;
                if (read(sockfd, &dataPkt, sizeof(packet)) <= 0) {
                    printf("DEBUG Server: Error reading data packet\n");
                    break;
                }
                
                if (dataPkt.type != DATA_PACKET) {
                    printf("DEBUG Server: Received unexpected packet type: %d\n", dataPkt.type);
                    break;
                }
                
                memcpy(fileData + bytesRead, dataPkt.payload, dataPkt.length);
                bytesRead += dataPkt.length;
                printf("DEBUG Server: Received data packet %d, progress: %zu/%u bytes (%d%%)\n", 
                       dataPkt.seqn, bytesRead, pkt.total_size, (int)(bytesRead * 100 / pkt.total_size));
            }
            
            printf("DEBUG Server: Finished receiving file data, saving file\n");
            
            // Save file
            pthread_mutex_lock(&fileMutex);
            bool success = fileManager.saveFile(username, filename, fileData, bytesRead);
            pthread_mutex_unlock(&fileMutex);
            
            printf("DEBUG Server: File save %s\n", success ? "successful" : "failed");
            
            delete[] fileData;
            
            if (success) {
                // Notify other clients about this file
                packet notifyPkt;
                notifyPkt.type = SYNC_NOTIFICATION;
                notifyPkt.seqn = 0;
                notifyPkt.total_size = 0;
                strcpy(notifyPkt.payload, filename.c_str());
                notifyPkt.length = filename.length();
                
                printf("DEBUG Server: Notifying other clients about file: %s\n", filename.c_str());
                notify_clients(username, notifyPkt, sockfd);
                
                // Send success response
                strcpy(response.payload, "OK");
                response.length = 2;
            } else {
                strcpy(response.payload, "ERROR");
                response.length = 5;
            }
            printf("DEBUG Server: Sending upload response: %s with seq: %d\n", response.payload, response.seqn);
            send(sockfd, &response, sizeof(packet), 0);
            break;
        }
        
        case CMD_DOWNLOAD: {
            std::string filename(pkt.payload);
            
            // Check if file exists
            pthread_mutex_lock(&fileMutex);
            bool exists = fileManager.fileExists(username, filename);
            
            if (exists) {
                // Get file size first
                size_t fileSize = 0;
                fileManager.getFile(username, filename, nullptr, fileSize);
                
                // Allocate buffer for file
                char* fileData = new char[fileSize];
                bool success = fileManager.getFile(username, filename, fileData, fileSize);
                pthread_mutex_unlock(&fileMutex);
                
                if (success) {
                    // Send response header
                    response.total_size = fileSize;
                    strcpy(response.payload, "OK");
                    response.length = 2;
                    printf("DEBUG Server: Sending download response: %s with seq: %d\n", response.payload, response.seqn);
                    send(sockfd, &response, sizeof(packet), 0);
                    
                    // Send file data in chunks
                    size_t bytesSent = 0;
                    while (bytesSent < fileSize) {
                        packet dataPkt;
                        dataPkt.type = DATA_PACKET;
                        // Include the original command's sequence number to help client thread identify these packets
                        dataPkt.seqn = response.seqn; 
                        
                        size_t bytesToSend = std::min(sizeof(dataPkt.payload), fileSize - bytesSent);
                        memcpy(dataPkt.payload, fileData + bytesSent, bytesToSend);
                        dataPkt.length = bytesToSend;
                        
                        send(sockfd, &dataPkt, sizeof(packet), 0);
                        bytesSent += bytesToSend;
                    }
                    
                    delete[] fileData;
                } else {
                    strcpy(response.payload, "ERROR");
                    response.length = 5;
                    printf("DEBUG Server: Sending download response: %s with seq: %d\n", response.payload, response.seqn);
                    send(sockfd, &response, sizeof(packet), 0);
                }
            } else {
                pthread_mutex_unlock(&fileMutex);
                strcpy(response.payload, "NOT_FOUND");
                response.length = 9;
                printf("DEBUG Server: Sending download response: %s with seq: %d\n", response.payload, response.seqn);
                send(sockfd, &response, sizeof(packet), 0);
            }
            break;
        }
        
        case CMD_DELETE: {
            std::string filename(pkt.payload);
            
            pthread_mutex_lock(&fileMutex);
            bool success = fileManager.deleteFile(username, filename);
            pthread_mutex_unlock(&fileMutex);
            
            if (success) {
                // Notify other clients
                packet notifyPkt;
                notifyPkt.type = SYNC_NOTIFICATION;
                notifyPkt.seqn = 1; // Use seqn=1 to indicate deletion
                notifyPkt.total_size = 0;
                strcpy(notifyPkt.payload, filename.c_str());
                notifyPkt.length = filename.length();
                
                notify_clients(username, notifyPkt, sockfd);
                
                strcpy(response.payload, "OK");
                response.length = 2;
            } else {
                strcpy(response.payload, "ERROR");
                response.length = 5;
            }
            printf("DEBUG Server: Sending delete response: %s with seq: %d\n", response.payload, response.seqn);
            send(sockfd, &response, sizeof(packet), 0);
            break;
        }
        
        case CMD_LIST_SERVER: {
            pthread_mutex_lock(&fileMutex);
            auto files = fileManager.listUserFiles(username);
            pthread_mutex_unlock(&fileMutex);
            
            // Format file list
            std::string fileList;
            for (const auto& file : files) {
                fileList += file.filename + "," + 
                           std::to_string(file.size) + "," + 
                           std::to_string(file.mtime) + "," + 
                           std::to_string(file.atime) + "," + 
                           std::to_string(file.ctime) + "\n";
            }
            
            // Send response
            response.total_size = fileList.size();
            strcpy(response.payload, fileList.substr(0, sizeof(response.payload) - 1).c_str());
            response.length = strlen(response.payload);
            printf("DEBUG Server: Sending list_server response with seq: %d\n", response.seqn);
            send(sockfd, &response, sizeof(packet), 0);
            
            // If list is longer than payload, send the rest in DATA packets
            if (fileList.size() > sizeof(response.payload)) {
                size_t bytesSent = sizeof(response.payload) - 1;
                while (bytesSent < fileList.size()) {
                    packet dataPkt;
                    dataPkt.type = DATA_PACKET;
                    // Use the same sequence number as the original command
                    dataPkt.seqn = response.seqn;
                    
                    size_t bytesToSend = std::min(sizeof(dataPkt.payload) - 1, fileList.size() - bytesSent);
                    strncpy(dataPkt.payload, fileList.c_str() + bytesSent, bytesToSend);
                    dataPkt.payload[bytesToSend] = '\0';
                    dataPkt.length = bytesToSend;
                    
                    send(sockfd, &dataPkt, sizeof(packet), 0);
                    bytesSent += bytesToSend;
                }
            }
            break;
        }
        
        case CMD_GET_SYNC_DIR: {
            pthread_mutex_lock(&fileMutex);
            fileManager.initUserDirectory(username);
            auto files = fileManager.listUserFiles(username);
            pthread_mutex_unlock(&fileMutex);
            
            // Send number of files
            response.total_size = files.size();
            strcpy(response.payload, "OK");
            response.length = 2;
            printf("DEBUG Server: Sending get_sync_dir response: %s with seq: %d, total_size: %u\n", 
                   response.payload, response.seqn, response.total_size);
            send(sockfd, &response, sizeof(packet), 0);
            
            // Send each file info
            for (const auto& file : files) {
                packet infoPkt;
                infoPkt.type = SYNC_NOTIFICATION;
                infoPkt.seqn = 0;
                infoPkt.total_size = file.size;
                strcpy(infoPkt.payload, file.filename.c_str());
                infoPkt.length = file.filename.length();
                
                send(sockfd, &infoPkt, sizeof(packet), 0);
            }
            break;
        }
        
        case CMD_EXIT: {
            strcpy(response.payload, "OK");
            response.length = 2;
            printf("DEBUG Server: Sending exit response: %s with seq: %d\n", response.payload, response.seqn);
            send(sockfd, &response, sizeof(packet), 0);
            break;
        }
        
        default:
            break;
    }
}
