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
    
    packet pkt;
    std::string username;
    
    // Wait for login packet first
    if (read(sockfd, &pkt, sizeof(packet)) > 0) {
        if (pkt.type == CMD_LOGIN) {
            username = pkt.payload;
            printf("Login de usuário: %s\n", username.c_str());
            
            // Register client
            register_client(username, sockfd);
            
            // Initialize user directory
            pthread_mutex_lock(&fileMutex);
            fileManager.initUserDirectory(username);
            pthread_mutex_unlock(&fileMutex);
            
            // Send login confirmation
            packet response;
            response.type = CMD_LOGIN;
            response.seqn = 0;
            response.total_size = 0;
            response.length = 0;
            write(sockfd, &response, sizeof(packet));
            
            // Process commands
            while (read(sockfd, &pkt, sizeof(packet)) > 0) {
                process_command(sockfd, pkt);
                
                if (pkt.type == CMD_EXIT) {
                    break;
                }
            }
        }
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
                write(client.sockfd, &pkt, sizeof(packet));
            }
        }
    }
    
    pthread_mutex_unlock(&clientsMutex);
}

void process_command(int sockfd, packet& pkt) {
    packet response;
    memset(&response, 0, sizeof(packet));
    response.type = pkt.type;
    response.seqn = pkt.seqn;
    
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
    
    switch (pkt.type) {
        case CMD_UPLOAD: {
            // Extract filename from the payload
            std::string filename(pkt.payload);
            
            // Receive file data
            char* fileData = new char[pkt.total_size];
            size_t bytesRead = 0;
            
            // Loop to receive all file data packets
            while (bytesRead < pkt.total_size) {
                packet dataPkt;
                if (read(sockfd, &dataPkt, sizeof(packet)) <= 0) {
                    break;
                }
                
                if (dataPkt.type != DATA_PACKET) {
                    break;
                }
                
                memcpy(fileData + bytesRead, dataPkt.payload, dataPkt.length);
                bytesRead += dataPkt.length;
            }
            
            // Save file
            pthread_mutex_lock(&fileMutex);
            bool success = fileManager.saveFile(username, filename, fileData, bytesRead);
            pthread_mutex_unlock(&fileMutex);
            
            delete[] fileData;
            
            if (success) {
                // Notify other clients about this file
                packet notifyPkt;
                notifyPkt.type = SYNC_NOTIFICATION;
                notifyPkt.seqn = 0;
                notifyPkt.total_size = 0;
                strcpy(notifyPkt.payload, filename.c_str());
                notifyPkt.length = filename.length();
                
                notify_clients(username, notifyPkt, sockfd);
                
                // Send success response
                strcpy(response.payload, "OK");
                response.length = 2;
            } else {
                strcpy(response.payload, "ERROR");
                response.length = 5;
            }
            write(sockfd, &response, sizeof(packet));
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
                    write(sockfd, &response, sizeof(packet));
                    
                    // Send file data in chunks
                    size_t bytesSent = 0;
                    while (bytesSent < fileSize) {
                        packet dataPkt;
                        dataPkt.type = DATA_PACKET;
                        dataPkt.seqn = bytesSent / sizeof(dataPkt.payload) + 1;
                        
                        size_t bytesToSend = std::min(sizeof(dataPkt.payload), fileSize - bytesSent);
                        memcpy(dataPkt.payload, fileData + bytesSent, bytesToSend);
                        dataPkt.length = bytesToSend;
                        
                        write(sockfd, &dataPkt, sizeof(packet));
                        bytesSent += bytesToSend;
                    }
                    
                    delete[] fileData;
                } else {
                    strcpy(response.payload, "ERROR");
                    response.length = 5;
                    write(sockfd, &response, sizeof(packet));
                }
            } else {
                pthread_mutex_unlock(&fileMutex);
                strcpy(response.payload, "NOT_FOUND");
                response.length = 9;
                write(sockfd, &response, sizeof(packet));
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
            write(sockfd, &response, sizeof(packet));
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
            write(sockfd, &response, sizeof(packet));
            
            // If list is longer than payload, send the rest in DATA packets
            if (fileList.size() > sizeof(response.payload)) {
                size_t bytesSent = sizeof(response.payload) - 1;
                while (bytesSent < fileList.size()) {
                    packet dataPkt;
                    dataPkt.type = DATA_PACKET;
                    dataPkt.seqn = bytesSent / sizeof(dataPkt.payload) + 1;
                    
                    size_t bytesToSend = std::min(sizeof(dataPkt.payload) - 1, fileList.size() - bytesSent);
                    strncpy(dataPkt.payload, fileList.c_str() + bytesSent, bytesToSend);
                    dataPkt.payload[bytesToSend] = '\0';
                    dataPkt.length = bytesToSend;
                    
                    write(sockfd, &dataPkt, sizeof(packet));
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
            write(sockfd, &response, sizeof(packet));
            
            // Send each file info
            for (const auto& file : files) {
                packet infoPkt;
                infoPkt.type = SYNC_NOTIFICATION;
                infoPkt.seqn = 0;
                infoPkt.total_size = file.size;
                strcpy(infoPkt.payload, file.filename.c_str());
                infoPkt.length = file.filename.length();
                
                write(sockfd, &infoPkt, sizeof(packet));
            }
            break;
        }
        
        case CMD_EXIT: {
            strcpy(response.payload, "OK");
            response.length = 2;
            write(sockfd, &response, sizeof(packet));
            break;
        }
        
        default:
            break;
    }
}
