#include "connection_handler.h"
#include "file_manager.h"
#include "packet.h"
#include "common.h"
#include "socket_utils.h"
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <sys/socket.h>
#include <netinet/tcp.h>  // For TCP_NODELAY and IPPROTO_TCP // macOS only?
// The following two headers are needed for TCP_NODELAY and IPPROTO_TCP, required for Linux
#include <netinet/in.h>
// #include <linux/tcp.h>
#include <fcntl.h>
#include <iostream>

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
bool register_client(const std::string& username, int sockfd);
void unregister_client(const std::string& username, int sockfd);
void notify_clients(const std::string& username, const packet& pkt, int excludeSockfd);
void process_command(int sockfd, packet& pkt);

void run_server(int port) {
    int err;
    int sockfd = create_socket();

    err = bind_socket(sockfd, port);
    if (err < 0) {
        DEBUG_PRINTF("error at bind socket, code %d\n", err);
    }

    err = listen_socket(sockfd);
    if (err < 0) {
        DEBUG_PRINTF("error at listen socket, code: %d\n", err);
    }


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
    DEBUG_PRINTF("DEBUG Server: Waiting to read login packet (size=%zu bytes)...\n", sizeof(packet));
    ssize_t direct_bytes = recv(sockfd, &pkt, sizeof(packet), MSG_WAITALL);
    DEBUG_PRINTF("DEBUG Server: Direct read returned %zd bytes\n", direct_bytes);

    if (direct_bytes > 0) {
        DEBUG_PRINTF("DEBUG Server: Received packet header - type: %d, seqn: %d, length: %d\n",
               pkt.type, pkt.seqn, pkt.length);

        // Validate payload length
        if (pkt.length > sizeof(pkt.payload)) {
            DEBUG_PRINTF("ERROR: Invalid payload length: %d (max: %zu)\n", pkt.length, sizeof(pkt.payload));
            close(sockfd);
            pthread_exit(NULL);
        }

        // Continue processing even with partial read as long as we have the essential header
        if (pkt.type == CMD_LOGIN) {
            // Ensure the payload is null-terminated
            pkt.payload[pkt.length < sizeof(pkt.payload) ? pkt.length : sizeof(pkt.payload) - 1] = '\0';
            username = pkt.payload;
            printf("Login de usuário: %s (seq: %d)\n", username.c_str(), pkt.seqn);

            // Register client and check session limit
            if (!register_client(username, sockfd)) {
                // Error message already printed by register_client
                // Optionally send error packet back to client before closing
                packet error_pkt;
                memset(&error_pkt, 0, sizeof(packet));
                error_pkt.type = CMD_EXIT; // Use EXIT type to signal client closure
                error_pkt.seqn = pkt.seqn; // Acknowledge the login attempt sequence
                const char* errMsg = "Session limit (2) reached";
                strncpy(error_pkt.payload, errMsg, sizeof(error_pkt.payload)-1);
                error_pkt.length = strlen(errMsg);
                send(sockfd, &error_pkt, sizeof(packet), 0); // Best effort send

                close(sockfd);
                pthread_exit(NULL); // Exit thread if registration failed
            }

            // Initialize user directory (only if registration succeeded)
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

            DEBUG_PRINTF("DEBUG Server: Sending login response with seq: %d\n", response.seqn);
            ssize_t bytes_sent = send(sockfd, &response, sizeof(packet), 0);
            DEBUG_PRINTF("DEBUG Server: Direct send returned %zd bytes\n", bytes_sent);

            // Process commands
            while (true) {
                memset(&pkt, 0, sizeof(packet)); // Clear packet before reading

                // Add a basic socket check before receiving
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
                    DEBUG_PRINTF("DEBUG Server: Socket error check failed, closing connection.\n");
                    break;
                }

                // Use direct socket operations to read command packets
                // DEBUG_PRINTF("DEBUG Server: Waiting for next command packet...\n");
                errno = 0; // Clear errno before the call

                // Use MSG_PEEK first to check if data is available without consuming it
                ssize_t peek_bytes = recv(sockfd, &pkt, sizeof(packet), MSG_PEEK | MSG_DONTWAIT);
                if (peek_bytes == 0) {
                    // Client closed connection gracefully
                    DEBUG_PRINTF("DEBUG Server: Client closed connection (received EOF).\n");
                    break;
                } else if (peek_bytes < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // No data available, just timeout, continue waiting
                        // DEBUG_PRINTF("DEBUG Server: Timeout waiting for command, retrying...\n");
                        continue;
                    } else {
                        // Real error
                        DEBUG_PRINTF("DEBUG Server: Error checking for data: %s (errno=%d)\n",
                               strerror(errno), errno);
                        break;
                    }
                }

                // Now do the actual read with MSG_WAITALL since we know data is available
                ssize_t cmd_bytes = recv(sockfd, &pkt, sizeof(packet), MSG_WAITALL);

                if (cmd_bytes <= 0) {
                    DEBUG_PRINTF("DEBUG Server: Failed to read command packet: %s (errno=%d)\n",
                           strerror(errno), errno);

                    // Check if connection is still alive
                    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
                        DEBUG_PRINTF("DEBUG Server: Socket is in error state, closing connection.\n");
                        break;
                    }

                    // If it's a timeout, we can try again
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        DEBUG_PRINTF("DEBUG Server: Timeout waiting for command, retrying...\n");
                        continue;
                    }

                    break;
                }

                if (cmd_bytes < sizeof(packet)) {
                    DEBUG_PRINTF("DEBUG Server: Read incomplete packet (%zd of %zu bytes)\n",
                           cmd_bytes, sizeof(packet));
                    break;
                }

                // Validate received packet
                if (pkt.type == 0) {
                    DEBUG_PRINTF("DEBUG Server: Received invalid packet with type=0, ignoring.\n");
                    continue;
                }

                DEBUG_PRINTF("DEBUG Server: Received command packet type: %d, seq: %d\n", pkt.type, pkt.seqn);

                // Process the command in a try/catch block to prevent crashes
                try {
                    process_command(sockfd, pkt);
                } catch (const std::exception& e) {
                    DEBUG_PRINTF("ERROR Server: Exception processing command: %s\n", e.what());
                    // Continue processing commands rather than disconnecting
                }

                if (pkt.type == CMD_EXIT) {
                    DEBUG_PRINTF("DEBUG Server: Received exit command, closing connection.\n");
                    break;
                }
            }
        } else {
            DEBUG_PRINTF("ERROR: Expected login packet (type 1), but received type %d\n", pkt.type);
        }
    } else {
        DEBUG_PRINTF("ERROR: Failed to read login packet: %s\n", strerror(errno));
    }

    // Unregister client on disconnect
    if (!username.empty()) {
        unregister_client(username, sockfd);
    }

    close(sockfd);
    printf("Cliente desconectado: %s\n", username.c_str());
    pthread_exit(NULL);
}

bool register_client(const std::string& username, int sockfd) {
    pthread_mutex_lock(&clientsMutex);

    // Check session limit
    if (connectedClients.count(username) && connectedClients[username].size() >= 2) {
        DEBUG_PRINTF("SERVER: Session limit (2) reached for user '%s'. Denying connection %d.\n", username.c_str(), sockfd);
        pthread_mutex_unlock(&clientsMutex);
        return false;
    }

    ClientInfo clientInfo;
    clientInfo.username = username;
    clientInfo.sockfd = sockfd;
    connectedClients[username].push_back(clientInfo);

    DEBUG_PRINTF("SERVER: Registered client %s on socket %d. Total sessions for user: %zu\n",
           username.c_str(), sockfd, connectedClients[username].size());

    pthread_mutex_unlock(&clientsMutex);
    return true;
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
    DEBUG_PRINTF("DEBUG Server: notify_clients called for user '%s', excludeSockfd=%d\n", username.c_str(), excludeSockfd);

    std::vector<int> sockets_to_notify;

    // Copy the list of sockets while holding the mutex for the minimum time
    pthread_mutex_lock(&clientsMutex);
    auto it = connectedClients.find(username);
    if (it != connectedClients.end()) {
        DEBUG_PRINTF("DEBUG Server: Found %zu connected clients for user '%s'\n", it->second.size(), username.c_str());
        for (const auto& client : it->second) {
            DEBUG_PRINTF("DEBUG Server: Considering client sockfd=%d (excludeSockfd=%d)\n", client.sockfd, excludeSockfd);
            if (client.sockfd != excludeSockfd) {
                sockets_to_notify.push_back(client.sockfd);
            }
        }
    } else {
        DEBUG_PRINTF("DEBUG Server: No connected clients found for user '%s'\n", username.c_str());
    }
    pthread_mutex_unlock(&clientsMutex);

    // Now send outside the critical section so a slow / blocked socket does
    // not freeze the whole server.
    for (int fd : sockets_to_notify) {
        DEBUG_PRINTF("DEBUG Server: Notifying client on socket %d about file: %s (packet type=%d, seqn=%d, length=%d)\n",
                     fd, pkt.payload, pkt.type, pkt.seqn, pkt.length);
        // Use non-blocking send with MSG_DONTWAIT so we never block indefinitely.
        ssize_t s = send(fd, &pkt, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (s < 0) {
            DEBUG_PRINTF("WARN: Failed to notify socket %d: %s\n", fd, strerror(errno));
        } else {
            DEBUG_PRINTF("DEBUG Server: Successfully notified socket %d (%zd bytes sent)\n", fd, s);
        }
    }
    DEBUG_PRINTF("DEBUG Server: notify_clients finished for user '%s'\n", username.c_str());
}

void process_command(int sockfd, packet& pkt) {
    // Create a fresh response packet for each command to avoid reusing memory
    packet response;
    memset(&response, 0, sizeof(packet)); // Clear the response packet

    // Basic setup - all commands need these
    response.type = pkt.type;
    response.seqn = pkt.seqn;  // Important: Use the same sequence number from the request
    response.total_size = 0;
    response.length = 0;

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
        DEBUG_PRINTF("ERROR: No username found for socket %d\n", sockfd);
        return;
    }

    DEBUG_PRINTF("DEBUG Server: Processing command type %d from user: %s, seq: %d\n",
           pkt.type, username.c_str(), pkt.seqn);

    // Process based on command type
    switch (pkt.type) {
        case CMD_UPLOAD: {
            // Extract filename from the payload
            std::string filename(pkt.payload);
            DEBUG_PRINTF("DEBUG Server: Received upload command for file: %s, size: %u bytes\n",
                   filename.c_str(), pkt.total_size);

            // Receive file data
            char* fileData = new char[pkt.total_size];
            size_t bytesRead = 0;

            // Loop to receive all file data packets
            while (bytesRead < pkt.total_size) {
                packet dataPkt;
                // Reliably read an entire packet. Using read_all protects against partial
                // TCP deliveries that occur when multiple sessions are active.
                size_t got = read_all(sockfd, &dataPkt, sizeof(packet));

                if (got != sizeof(packet)) {
                    DEBUG_PRINTF("DEBUG Server: Error reading data packet (read %zu of %zu bytes)\n", got, sizeof(packet));
                    break;
                }

                // Basic sanity-check of the packet
                if (dataPkt.type != DATA_PACKET || dataPkt.length > sizeof(dataPkt.payload)) {
                    DEBUG_PRINTF("DEBUG Server: Malformed data packet received (type=%d, length=%d)\n", dataPkt.type, dataPkt.length);
                    break;
                }

                memcpy(fileData + bytesRead, dataPkt.payload, dataPkt.length);
                bytesRead += dataPkt.length;
                DEBUG_PRINTF("DEBUG Server: Received data packet %d, progress: %zu/%u bytes (%d%%)\n",
                       dataPkt.seqn, bytesRead, pkt.total_size, (int)(bytesRead * 100 / pkt.total_size));
            }

            DEBUG_PRINTF("DEBUG Server: Finished receiving file data, saving file\n");

            // Save file
            pthread_mutex_lock(&fileMutex);
            bool success = fileManager.saveFile(username, filename, fileData, bytesRead);
            pthread_mutex_unlock(&fileMutex);

            DEBUG_PRINTF("DEBUG Server: File save %s\n", success ? "successful" : "failed");

            delete[] fileData;

            if (success) {
                // Notify other clients about this file
                packet notifyPkt;
                notifyPkt.type = SYNC_NOTIFICATION;
                notifyPkt.seqn = 0;
                notifyPkt.total_size = 0;
                std::string payload = std::string("U:") + filename;
                strncpy(notifyPkt.payload, payload.c_str(), sizeof(notifyPkt.payload) - 1);
                notifyPkt.payload[sizeof(notifyPkt.payload)-1] = '\0';
                notifyPkt.length = strlen(notifyPkt.payload);

                DEBUG_PRINTF("DEBUG Server: Notifying other clients about file: %s\n", filename.c_str());
                notify_clients(username, notifyPkt, sockfd);

                // Send success response
                strcpy(response.payload, "OK");
                response.length = 2;
            } else {
                strcpy(response.payload, "ERROR");
                response.length = 5;
            }
            DEBUG_PRINTF("DEBUG Server: Sending upload response: %s with seq: %d\n", response.payload, response.seqn);
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
                    DEBUG_PRINTF("DEBUG Server: Sending download response: %s with seq: %d\n", response.payload, response.seqn);
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
                    DEBUG_PRINTF("DEBUG Server: Sending download response: %s with seq: %d\n", response.payload, response.seqn);
                    send(sockfd, &response, sizeof(packet), 0);
                }
            } else {
                pthread_mutex_unlock(&fileMutex);
                strcpy(response.payload, "NOT_FOUND");
                response.length = 9;
                DEBUG_PRINTF("DEBUG Server: Sending download response: %s with seq: %d\n", response.payload, response.seqn);
                send(sockfd, &response, sizeof(packet), 0);
            }
            break;
        }

        case CMD_DELETE: {
            std::string filename(pkt.payload);

            // CRITICAL: Use command-specific variables to avoid shared memory issues
            int delete_client_fd = sockfd;
            uint16_t delete_seq = pkt.seqn;

            DEBUG_PRINTF("DEBUG Server: [DELETE] Command received for file: %s (seq: %d)\n",
                  filename.c_str(), delete_seq);

            // Prepare an isolated response packet
            packet delete_response;
            memset(&delete_response, 0, sizeof(packet));
            delete_response.type = CMD_DELETE;  // CRITICAL: set correct type
            delete_response.seqn = delete_seq;  // CRITICAL: preserve sequence number

            // Process delete operation with exclusive lock
            pthread_mutex_lock(&fileMutex);
            bool exists = fileManager.fileExists(username, filename);
            bool success = false;

            if (exists) {
                DEBUG_PRINTF("DEBUG Server: [DELETE] File %s exists, attempting deletion\n", filename.c_str());
                success = fileManager.deleteFile(username, filename);
            } else {
                DEBUG_PRINTF("DEBUG Server: [DELETE] File %s not found\n", filename.c_str());
            }
            pthread_mutex_unlock(&fileMutex);

            // Set response based on operation result
            if (!exists) {
                strcpy(delete_response.payload, "NOT_FOUND");
                delete_response.length = 9;
            } else if (success) {
                strcpy(delete_response.payload, "OK");
                delete_response.length = 2;

                // Only notify if deletion was successful
                packet notifyPkt;
                memset(&notifyPkt, 0, sizeof(packet));
                notifyPkt.type = SYNC_NOTIFICATION;
                notifyPkt.seqn = 0;
                std::string payload = std::string("D:") + filename;
                strncpy(notifyPkt.payload, payload.c_str(), sizeof(notifyPkt.payload) - 1);
                notifyPkt.payload[sizeof(notifyPkt.payload)-1] = '\0';
                notifyPkt.length = strlen(notifyPkt.payload);

                DEBUG_PRINTF("DEBUG Server: [DELETE] Notifying other clients about deletion\n");
                notify_clients(username, notifyPkt, delete_client_fd);
            } else {
                strcpy(delete_response.payload, "ERROR");
                delete_response.length = 5;
            }

            // Verify the response packet is correct
            DEBUG_PRINTF("DEBUG Server: [DELETE] Prepared response packet: type=%d, seq=%d, payload='%s'\n",
                  delete_response.type, delete_response.seqn, delete_response.payload);

            // CRITICAL: Send response directly with exclusive lock
            pthread_mutex_lock(&fileMutex);  // Use file mutex to ensure exclusive socket access
            ssize_t bytes_sent = send(delete_client_fd, &delete_response, sizeof(packet), 0);
            pthread_mutex_unlock(&fileMutex);

            if (bytes_sent == sizeof(packet)) {
                DEBUG_PRINTF("DEBUG Server: [DELETE] Response sent successfully (%zd bytes)\n", bytes_sent);
            } else {
                DEBUG_PRINTF("ERROR Server: [DELETE] Failed to send response (%zd bytes): %s\n",
                       bytes_sent, strerror(errno));
            }

            break;
        }

        case CMD_LIST_SERVER: {
            // Create local variables for this command
            packet list_response;
            memset(&list_response, 0, sizeof(packet));
            list_response.type = CMD_LIST_SERVER;
            list_response.seqn = pkt.seqn;

            DEBUG_PRINTF("DEBUG Server: Processing list_server command for user %s\n", username.c_str());

            // Force filesystem refresh by ensuring the user directory exists
            pthread_mutex_lock(&fileMutex);
            fileManager.initUserDirectory(username); // This refreshes directory access
            pthread_mutex_unlock(&fileMutex);

            // Get files with mutex protection
            pthread_mutex_lock(&fileMutex);
            auto files = fileManager.listUserFiles(username);
            pthread_mutex_unlock(&fileMutex);

            DEBUG_PRINTF("DEBUG Server: Found %zu files for user %s\n", files.size(), username.c_str());

            // Format file list
            std::string fileList;
            for (const auto& file : files) {
                std::string entry = file.filename + "," +
                           std::to_string(file.size) + "," +
                           std::to_string(file.mtime) + "," +
                           std::to_string(file.atime) + "," +
                           std::to_string(file.ctime) + "\n";
                fileList += entry;
                DEBUG_PRINTF("DEBUG Server: Adding file to list: %s (size: %zu)\n",
                       file.filename.c_str(), file.size);
            }

            // Set response metadata
            list_response.total_size = fileList.size();

            // Copy data to payload (up to the limit)
            if (fileList.empty()) {
                strcpy(list_response.payload, "");
                list_response.length = 0;
                DEBUG_PRINTF("DEBUG Server: No files found, sending empty list\n");
            } else {
                // Only copy up to the payload size limit
                size_t bytes_to_copy = std::min(sizeof(list_response.payload) - 1, fileList.size());
                strncpy(list_response.payload, fileList.c_str(), bytes_to_copy);
                list_response.payload[bytes_to_copy] = '\0'; // Ensure null termination
                list_response.length = bytes_to_copy;
                DEBUG_PRINTF("DEBUG Server: File list prepared (%zu bytes, %zu files)\n",
                       fileList.size(), files.size());
            }

            DEBUG_PRINTF("DEBUG Server: Sending list_server response with seq: %d, total_size: %u\n",
                   list_response.seqn, list_response.total_size);
            ssize_t bytes_sent = send(sockfd, &list_response, sizeof(packet), 0);
            DEBUG_PRINTF("DEBUG Server: Sent list_server response header (%zd bytes)\n", bytes_sent);

            // If list is longer than payload, send the rest in DATA packets
            if (fileList.size() > sizeof(list_response.payload) - 1) {
                size_t bytesSent = sizeof(list_response.payload) - 1;

                while (bytesSent < fileList.size()) {
                    packet dataPkt;
                    memset(&dataPkt, 0, sizeof(packet));
                    dataPkt.type = DATA_PACKET;
                    dataPkt.seqn = list_response.seqn;

                    size_t bytesToSend = std::min(sizeof(dataPkt.payload) - 1, fileList.size() - bytesSent);
                    strncpy(dataPkt.payload, fileList.c_str() + bytesSent, bytesToSend);
                    dataPkt.payload[bytesToSend] = '\0'; // Ensure null termination
                    dataPkt.length = bytesToSend;

                    DEBUG_PRINTF("DEBUG Server: Sending list_server data packet: %zu bytes\n", bytesToSend);
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
            DEBUG_PRINTF("DEBUG Server: Sending get_sync_dir response: %s with seq: %d, total_size: %u\n",
                   response.payload, response.seqn, response.total_size);
            send(sockfd, &response, sizeof(packet), 0);

            // Send each file info
            for (const auto& file : files) {
                packet infoPkt;
                infoPkt.type = SYNC_NOTIFICATION;
                infoPkt.seqn = 0;
                infoPkt.total_size = file.size;
                std::string payload = std::string("U:") + file.filename;
                strncpy(infoPkt.payload, payload.c_str(), sizeof(infoPkt.payload) - 1);
                infoPkt.payload[sizeof(infoPkt.payload)-1] = '\0';
                infoPkt.length = strlen(infoPkt.payload);

                send(sockfd, &infoPkt, sizeof(packet), 0);
            }
            break;
        }

        case CMD_EXIT: {
            strcpy(response.payload, "OK");
            response.length = 2;
            DEBUG_PRINTF("DEBUG Server: Sending exit response: %s with seq: %d\n", response.payload, response.seqn);
            send(sockfd, &response, sizeof(packet), 0);
            break;
        }

        default:
            break;
    }
}
