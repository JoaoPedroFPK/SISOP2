#include "sync.h"
#include "socket_utils.h"
#include "common.h"
#include "packet.h"  // Explicit include to guarantee visibility of struct packet
#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>      // For fcntl
#include <dirent.h>
#include <unistd.h>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <map>
#include <condition_variable>
#include <errno.h>
#include <atomic>

namespace fs = std::filesystem;

// Global variables
int server_socket = -1;
std::string sync_dir_path;
std::string current_username;

// Store server endpoint so we can attempt to reconnect later
static std::string g_server_ip;
static int g_server_port = 0;

// Flag that tells every thread whether the TCP connection is still alive
static std::atomic<bool> connection_alive{true};

// Mutex for file operations
std::mutex file_mutex;

// Mutex for socket communication
std::mutex socket_mutex;

std::mutex download_mutex;

// Map to store responses
std::map<uint16_t, packet> responses;
std::mutex responses_mutex;
std::condition_variable responses_cv;
uint16_t next_seq_number = 1;

// Monitor thread coordination
std::mutex monitor_ready_mutex;
std::condition_variable monitor_ready_cv;
bool monitor_thread_ready = false;

// File modification times to track changes
std::unordered_map<std::string, time_t> file_mtimes;

// Enum for packet types (same as server)
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

// Forward declarations
void initialize_sync();
void monitor_server_notifications();
void check_for_file_changes();
void process_file_change(const std::string& filename, bool is_deleted);
void update_file_mtimes();
bool reset_socket_connection();

bool sync_start(const char* username, const char* server_ip, int port) {
    printf("Iniciando sessão para o usuário %s...\n", username);

    // Save username
    current_username = username;

    // Create sync directory path
    sync_dir_path = "sync_dir_" + current_username;

    // Reset monitor thread ready flag
    {
        std::lock_guard<std::mutex> lock(monitor_ready_mutex);
        monitor_thread_ready = false;
    }

    // Connect to server
    server_socket = create_socket();

    // Set socket to non-blocking mode for better timeout handling
    int flags = fcntl(server_socket, F_GETFL, 0);
    fcntl(server_socket, F_SETFL, flags & ~O_NONBLOCK); // Ensure blocking mode

    // Set socket timeout options
    struct timeval timeout;
    timeout.tv_sec = 30;  // Increase from 5 to 30 seconds
    timeout.tv_usec = 0;

    // Set send and receive timeout
    if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("Error setting receive timeout");
    }
    if (setsockopt(server_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("Error setting send timeout");
    }

    // Set TCP_NODELAY to disable Nagle's algorithm
    int flag = 1;
    if (setsockopt(server_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        perror("Error setting TCP_NODELAY");
    }

    g_server_ip = server_ip;
    g_server_port = port;

    int err = connect_socket(server_socket, server_ip, port);
    if (err < 0) {
        // perror("Erro ao conectar ao servidor");
        DEBUG_PRINTF("Error ao conectar ao sevidor, err: %d\n", err);
        return false;
    }

    printf("Conectado ao servidor %s:%d\n", server_ip, port);

    // Make sure sync directory exists before we start
    watch_sync_dir();

    // Start threads for monitoring BEFORE sending any commands
    std::thread server_thread(monitor_server_notifications);
    std::thread file_thread(check_for_file_changes);

    // Wait for monitor thread to be ready
    {
        std::lock_guard<std::mutex> lock(monitor_ready_mutex);
        monitor_thread_ready = true;  // Set ready flag directly since we're not starting the monitor thread
        monitor_ready_cv.notify_all();
    }

    // Now send login packet
    packet login_pkt;
    memset(&login_pkt, 0, sizeof(packet)); // Clear the entire structure
    login_pkt.type = CMD_LOGIN;
    login_pkt.seqn = get_next_seq(); // Use proper sequence number
    login_pkt.total_size = 0;
    strcpy(login_pkt.payload, username);
    login_pkt.length = strlen(username);

    DEBUG_PRINTF("DEBUG: Sending login packet with seq: %d, type: %d, length: %d\n",
           login_pkt.seqn, login_pkt.type, login_pkt.length);
    DEBUG_PRINTF("DEBUG: Packet size: %zu bytes\n", sizeof(packet));

    // Use direct socket send for login packet
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t bytes_sent = send(server_socket, &login_pkt, sizeof(packet), 0);
        if (bytes_sent <= 0) {
            DEBUG_PRINTF("ERROR: Failed to send login packet: %s\n", strerror(errno));
            close(server_socket);
            return false;
        }
        DEBUG_PRINTF("DEBUG: Sent %zd bytes directly via socket\n", bytes_sent);
    }

    // Wait for server response using direct socket operations
    packet response;
    memset(&response, 0, sizeof(packet));

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        DEBUG_PRINTF("DEBUG: Waiting for login response...\n");
        ssize_t bytes_received = recv(server_socket, &response, sizeof(packet), MSG_WAITALL);
        if (bytes_received <= 0) {
            DEBUG_PRINTF("ERROR: Failed to receive login response: %s\n", strerror(errno));
            close(server_socket);
            return false;
        }
        DEBUG_PRINTF("DEBUG: Received %zd bytes login response\n", bytes_received);
    }

    DEBUG_PRINTF("DEBUG: Received login response with seq: %d, type: %d\n", response.seqn, response.type);

    if (response.type == CMD_LOGIN) {
        printf("Login bem-sucedido.\n");

        // Initialize sync (Initial sync handshake)
        DEBUG_PRINTF("Realizando sincronização inicial...\n");
        get_sync_dir();
        printf("Sincronização inicial concluída.\n");

        // Detach threads to run in background
        server_thread.detach();
        file_thread.detach();

        printf("Sincronização iniciada. Use os comandos para interagir.\n");

        connection_alive.store(true);

        // Successful start
        return true;
    } else {
        printf("Erro na resposta de login: tipo inesperado %d\n", response.type);
        close(server_socket);
        return false; // Exit if login failed
    }
}

bool sync_dir_exists() {
    struct stat info;
    return stat(sync_dir_path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

void create_sync_dir() {
    mkdir(sync_dir_path.c_str(), 0755);
}

void watch_sync_dir() {
    if (!sync_dir_exists()) {
        create_sync_dir();
    }
}

void initialize_sync() {
    // Make sure the sync directory exists
    watch_sync_dir();

    // Get file list from server
    get_sync_dir();

    // Initialize file mtimes for monitoring changes
    update_file_mtimes();
}

void update_file_mtimes() {
    std::lock_guard<std::mutex> lock(file_mutex);
    file_mtimes.clear();

    DIR* dir = opendir(sync_dir_path.c_str());
    if (dir == nullptr) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_REG) {  // Regular file
            std::string filename = entry->d_name;
            std::string filepath = sync_dir_path + "/" + filename;

            struct stat st;
            if (stat(filepath.c_str(), &st) == 0) {
                file_mtimes[filename] = st.st_mtime;
            }
        }
    }

    closedir(dir);
}

void check_for_file_changes() {
    while (true) {
        // Sleep for a short time to reduce CPU usage
        std::this_thread::sleep_for(std::chrono::seconds(1));

        if (!sync_dir_exists()) {
            continue;
        }

        std::unordered_map<std::string, time_t> current_mtimes;
        std::vector<std::string> current_files;

        // Get current files and mtimes
        DIR* dir = opendir(sync_dir_path.c_str());
        if (dir == nullptr) {
            continue;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_REG) {  // Regular file
                std::string filename = entry->d_name;
                std::string filepath = sync_dir_path + "/" + filename;
                current_files.push_back(filename);

                struct stat st;
                if (stat(filepath.c_str(), &st) == 0) {
                    current_mtimes[filename] = st.st_mtime;
                }
            }
        }

        closedir(dir);

        // Check for modified or new files
        for (const auto& file : current_files) {
            auto it = file_mtimes.find(file);
            if (it == file_mtimes.end() || it->second != current_mtimes[file]) {
                // File is new or modified
                process_file_change(file, false);
            }
        }

        // Check for deleted files
        for (const auto& entry : file_mtimes) {
            if (current_mtimes.find(entry.first) == current_mtimes.end()) {
                // File was deleted
                process_file_change(entry.first, true);
            }
        }

        // Update mtimes
        file_mutex.lock();
        file_mtimes = current_mtimes;
        file_mutex.unlock();
    }
}

void process_file_change(const std::string& filename, bool is_deleted) {
    if (is_deleted) {
        // Delete file on server
        delete_file(filename);
    } else {
        // Upload file to server
        std::string filepath = sync_dir_path + "/" + filename;
        upload_file(filepath);
    }
}

// Function to send a command and get a response
packet send_command_and_wait(const packet& cmd) {
    uint16_t seq = cmd.seqn;

    // Send the command with the socket mutex locked
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        write(server_socket, &cmd, sizeof(packet));
    }

    // Wait for response with the sequence number
    std::unique_lock<std::mutex> lock(responses_mutex);
    while (responses.find(seq) == responses.end()) {
        responses_cv.wait(lock);
    }

    // Get the response and remove it from the map
    packet response = responses[seq];
    responses.erase(seq);

    return response;
}

// Get next sequence number
uint16_t get_next_seq() {
    std::lock_guard<std::mutex> lock(responses_mutex);
    return next_seq_number++;
}

// Function to monitor notifications from the server
void monitor_server_notifications() {
    bool command_completed = false;
    uint16_t expected_seqn = 0;
    size_t files_remaining = 0;

    DEBUG_PRINTF("DEBUG Monitor: Thread starting\n");

    // Signal that the monitor thread is ready
    {
        std::lock_guard<std::mutex> lock(monitor_ready_mutex);
        monitor_thread_ready = true;
        monitor_ready_cv.notify_all();
    }

    DEBUG_PRINTF("DEBUG Monitor: Thread ready - waiting for packets\n");

    while (true) {
        // Wait for notifications from the server
        std::lock_guard<std::mutex> pause_monitor(download_mutex);
        
        packet pkt;
        memset(&pkt, 0, sizeof(packet)); // Clear the packet before reading

        {
            std::lock_guard<std::mutex> lock(socket_mutex);

            // Check if there's any bytes available to read
            int bytes_available = 0;
            int err = ioctl(server_socket, FIONREAD, &bytes_available);
            if (err < 0) {
                DEBUG_PRINTF("ERR: %d\n", err);
            }

            if (bytes_available > 0) {

                size_t bytes_read = read_all(server_socket, &pkt, sizeof(packet));
                if (bytes_read > 0 && bytes_read != sizeof(packet)) {
                    DEBUG_PRINTF("ERROR: Lost connection to server (read %zu of %zu).\n",
                        bytes_read, sizeof(packet));
                    connection_alive.store(false);
                    break; // Exit monitor thread; other threads will notice connection loss
                }
            }
        }

        if (pkt.length != 0) {
            DEBUG_PRINTF("DEBUG Monitor: Received packet type: %d, seq: %d, length: %d, payload: %.10s...\n",
                   pkt.type, pkt.seqn, pkt.length, pkt.payload);
        }

        // Handle the packet based on its type
        if (pkt.type == SYNC_NOTIFICATION) {
            DEBUG_PRINTF("DEBUG Monitor: Processing sync notification\n");

            // Check if this is part of a get_sync_dir response
            if (command_completed && files_remaining > 0) {
                files_remaining--;
                DEBUG_PRINTF("DEBUG Monitor: Processing sync dir file (%zu remaining)\n", files_remaining);

                if (files_remaining == 0) {
                    // Signal that the get_sync_dir command is fully complete
                    command_completed = false;
                    DEBUG_PRINTF("DEBUG Monitor: All sync dir files processed\n");
                }
                // Process the file notification regardless
                handle_server_notification(pkt);
            } else {
                // It's a standalone sync notification from another device
                DEBUG_PRINTF("DEBUG Monitor: Processing notification from another device\n");
                handle_server_notification(pkt);
            }
        }
        else { // It's a response to a command
            // DEBUG_PRINTF("DEBUG Monitor: Received command response, adding to responses map\n");

            // Special handling for get_sync_dir
            if (pkt.type == CMD_GET_SYNC_DIR) {
                // Mark that we're expecting file notifications
                if (strcmp(pkt.payload, "OK") == 0) {
                    command_completed = true;
                    files_remaining = pkt.total_size;
                    expected_seqn = pkt.seqn;
                    // DEBUG_PRINTF("DEBUG Monitor: get_sync_dir response OK, expecting %zu files\n", files_remaining);

                    // If no files to sync, mark command as done immediately
                    if (files_remaining == 0) {
                        command_completed = false;
                        DEBUG_PRINTF("DEBUG Monitor: No files to sync, command completed\n");
                    }
                }
            }

            // Store the response in the map
            {
                std::lock_guard<std::mutex> lock(responses_mutex);
                responses[pkt.seqn] = pkt;
                // DEBUG_PRINTF("DEBUG Monitor: Added response for seq %d to map (map size: %zu)\n",
                //       pkt.seqn, responses.size());
                responses_cv.notify_all();
            }
        }
    }
}

void handle_server_notification(packet& pkt) {
    DEBUG_PRINTF("DEBUG: Handling server notification type %d\n", pkt.type);

    // Lock file operations during handling
    std::lock_guard<std::mutex> lock(file_mutex);

    if (pkt.type == SYNC_NOTIFICATION) {
        // Payload format: <action>:<filename>
        // action: 'U' for upload/update, 'D' for delete
        std::string payload_str(pkt.payload, pkt.length);
        size_t delimiter_pos = payload_str.find(':');

        if (delimiter_pos != std::string::npos) {
            char action = payload_str[0];
            std::string filename = payload_str.substr(delimiter_pos + 1);
            std::string full_path = sync_dir_path + "/" + filename;

            DEBUG_PRINTF("DEBUG: Received notification - Action: %c, File: %s\n", action, filename.c_str());

            if (action == 'U') {
                // Request download from server
                DEBUG_PRINTF("Atualização detectada no servidor para %s. Baixando...\n", filename.c_str());

                // Create download request
                packet download_req;
                memset(&download_req, 0, sizeof(packet));
                download_req.type = CMD_DOWNLOAD;
                download_req.seqn = get_next_seq();
                strncpy(download_req.payload, filename.c_str(), sizeof(download_req.payload) - 1);
                download_req.length = strlen(download_req.payload);

                // Send download request
                {
                    std::lock_guard<std::mutex> sock_lock(socket_mutex);
                    if (write_all(server_socket, &download_req, sizeof(packet)) != sizeof(packet)) {
                         DEBUG_PRINTF("ERROR: Failed to send download request for notification %s\n", filename.c_str());
                         return;
                    }

                    // Wait for server response
                    packet response;
                    memset(&response, 0, sizeof(packet));
                    if (read_all(server_socket, &response, sizeof(packet)) != sizeof(packet)) {
                        DEBUG_PRINTF("ERROR: Failed to receive download response\n");
                        return;
                    }

                    if (strcmp(response.payload, "OK") != 0) {
                        DEBUG_PRINTF("ERROR: Server returned error for download: %s\n", response.payload);
                        return;
                    }

                    // Prepare to receive file data
                    size_t fileSize = response.total_size;
                    DEBUG_PRINTF("DEBUG: Receiving file %s (%zu bytes) via notification handler\n", filename.c_str(), fileSize);

                    // Allocate buffer for file data
                    char* fileData = new char[fileSize];
                    size_t bytesRead = 0;

                    // Receive file data packets
                    while (bytesRead < fileSize) {
                        packet dataPkt;
                        memset(&dataPkt, 0, sizeof(packet));

                        if (read_all(server_socket, &dataPkt, sizeof(packet)) != sizeof(packet)) {
                            DEBUG_PRINTF("ERROR: Failed to receive file data packet\n");
                            delete[] fileData;
                            return;
                        }

                        if (dataPkt.type != DATA_PACKET) {
                            DEBUG_PRINTF("ERROR: Expected DATA_PACKET but got type %d\n", dataPkt.type);
                            delete[] fileData;
                            return;
                        }

                        memcpy(fileData + bytesRead, dataPkt.payload, dataPkt.length);
                        bytesRead += dataPkt.length;
                        DEBUG_PRINTF("DEBUG: Download progress: %zu/%zu bytes (%d%%)\n",
                               bytesRead, fileSize, (int)(bytesRead * 100 / fileSize));
                    }

                    // Write file to sync directory
                    std::ofstream file(full_path, std::ios::binary);
                    if (!file) {
                        DEBUG_PRINTF("ERROR: Failed to create file %s\n", full_path.c_str());
                        delete[] fileData;
                        return;
                    }

                    file.write(fileData, fileSize);
                    file.close();
                    delete[] fileData;

                    // Update file times map
                    struct stat st;
                    if (stat(full_path.c_str(), &st) == 0) {
                        file_mtimes[filename] = st.st_mtime;
                    }

                    printf("Arquivo %s baixado com sucesso via notificação.\n", filename.c_str());
                } // Release socket lock

            } else if (action == 'D') {
                // Delete the file locally
                printf("Arquivo %s removido no servidor. Removendo localmente...\n", filename.c_str());
                if (remove(full_path.c_str()) == 0) {
                    printf("Arquivo %s removido localmente.\n", filename.c_str());
                    // Update local mtimes map
                    file_mtimes.erase(filename);
                } else {
                    // Check if file didn't exist locally already (not an error)
                    if (errno != ENOENT) {
                        perror("Erro ao remover arquivo localmente");
                    }
                }
            }
        } else {
            DEBUG_PRINTF("ERROR: Invalid notification format: %s\n", payload_str.c_str());
        }
    } else {
         DEBUG_PRINTF("WARN: Received unexpected packet type %d in handle_server_notification\n", pkt.type);
    }
    // Mutex automatically released when lock goes out of scope
}

bool upload_file(const std::string& filepath) {
    // Check socket status first
    if (!check_socket_status()) {
        DEBUG_PRINTF("ERROR: Socket is in invalid state. Cannot send upload command.\n");
        return false;
    }

    // Get just the filename from the path
    std::string filename = fs::path(filepath).filename().string();

    DEBUG_PRINTF("DEBUG: Attempting to upload file: %s\n", filepath.c_str());

    // Check if file exists
    if (!fs::exists(filepath)) {
        printf("Arquivo '%s' não encontrado.\n", filepath.c_str());
        return false;
    }

    DEBUG_PRINTF("DEBUG: File exists, opening...\n");

    // Open the file
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        printf("Erro ao abrir o arquivo '%s'.\n", filepath.c_str());
        return false;
    }

    // Get file size
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    DEBUG_PRINTF("DEBUG: File size: %zu bytes\n", fileSize);

    // Read file content
    char* fileData = new char[fileSize];
    file.read(fileData, fileSize);
    file.close();

    DEBUG_PRINTF("DEBUG: File read into memory\n");

    // Send upload command
    packet cmd;
    memset(&cmd, 0, sizeof(packet)); // Clear packet
    cmd.type = CMD_UPLOAD;
    cmd.seqn = get_next_seq();
    cmd.total_size = fileSize;
    strcpy(cmd.payload, filename.c_str());
    cmd.length = filename.length();

    DEBUG_PRINTF("DEBUG: Sending upload command packet for file: %s, size: %zu\n",
           filename.c_str(), fileSize);

    // Send the command
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t bytes_sent = send(server_socket, &cmd, sizeof(packet), 0);
        if (bytes_sent <= 0) {
            DEBUG_PRINTF("ERROR: Failed to send upload command: %s\n", strerror(errno));
            delete[] fileData;
            return false;
        }
        DEBUG_PRINTF("DEBUG: Sent %zd bytes (upload command)\n", bytes_sent);
    }

    DEBUG_PRINTF("DEBUG: Upload command sent, sending file data...\n");

    // Send file data in chunks
    size_t bytesSent = 0;
    while (bytesSent < fileSize) {
        packet dataPkt;
        memset(&dataPkt, 0, sizeof(packet)); // Clear packet
        dataPkt.type = DATA_PACKET;
        dataPkt.seqn = bytesSent / sizeof(dataPkt.payload) + 1;

        size_t bytesToSend = std::min(sizeof(dataPkt.payload), fileSize - bytesSent);
        memcpy(dataPkt.payload, fileData + bytesSent, bytesToSend);
        dataPkt.length = bytesToSend;

        DEBUG_PRINTF("DEBUG: Sending data packet %d, bytes: %zu\n", dataPkt.seqn, bytesToSend);

        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            ssize_t bytes_sent = send(server_socket, &dataPkt, sizeof(packet), 0);
            if (bytes_sent <= 0) {
                DEBUG_PRINTF("ERROR: Failed to send file data: %s\n", strerror(errno));
                delete[] fileData;
                return false;
            }
            DEBUG_PRINTF("DEBUG: Sent %zd bytes (data packet)\n", bytes_sent);
        }

        bytesSent += bytesToSend;
        DEBUG_PRINTF("DEBUG: Progress: %zu/%zu bytes sent (%d%%)\n",
               bytesSent, fileSize, (int)(bytesSent * 100 / fileSize));
    }

    delete[] fileData;

    DEBUG_PRINTF("DEBUG: All file data sent, waiting for server response...\n");

    // Receive server response directly
    packet response;
    memset(&response, 0, sizeof(packet)); // Clear response packet

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        DEBUG_PRINTF("DEBUG: Waiting for upload response...\n");
        ssize_t recv_bytes = recv(server_socket, &response, sizeof(packet), MSG_WAITALL);
        if (recv_bytes <= 0) {
            DEBUG_PRINTF("ERROR: Failed to receive upload response: %s\n", strerror(errno));
            return false;
        }
        DEBUG_PRINTF("DEBUG: Received %zd bytes upload response\n", recv_bytes);
    }

    DEBUG_PRINTF("DEBUG: Received upload response: %s\n", response.payload);

    if (strcmp(response.payload, "OK") == 0) {
        printf("Arquivo '%s' enviado com sucesso.\n", filename.c_str());

        // Check if file was copied to sync directory
        std::string syncPath = sync_dir_path + "/" + filename;
        DEBUG_PRINTF("DEBUG: Checking if file was copied to sync directory: %s\n", syncPath.c_str());

        if (fs::exists(syncPath)) {
            DEBUG_PRINTF("DEBUG: File exists in sync directory\n");
        } else {
            DEBUG_PRINTF("DEBUG: File does NOT exist in sync directory\n");

            // Try to copy the file to sync directory if it's not there
            try {
                fs::copy_file(filepath, syncPath, fs::copy_options::overwrite_existing);
                DEBUG_PRINTF("DEBUG: Manually copied file to sync directory\n");
            } catch (const std::exception& e) {
                DEBUG_PRINTF("DEBUG: Error copying file to sync directory: %s\n", e.what());
            }
        }

        return true;
    } else if (response.type == SYNC_NOTIFICATION || strncmp(response.payload, "U:", 2) == 0) {
        // This is a notification, not an error
        printf("Arquivo '%s' enviado com sucesso. (Notificação recebida)\n", filename.c_str());

        // Make sure the file is in the sync directory
        std::string syncPath = sync_dir_path + "/" + filename;
        if (!fs::exists(syncPath)) {
            try {
                fs::copy_file(filepath, syncPath, fs::copy_options::overwrite_existing);
                DEBUG_PRINTF("DEBUG: Copied file to sync directory after notification\n");
            } catch (const std::exception& e) {
                DEBUG_PRINTF("DEBUG: Error copying file to sync directory: %s\n", e.what());
            }
        }

        return true;
    } else {
        printf("Erro ao enviar arquivo: %s\n", response.payload);
        return false;
    }
}

bool download_file(const std::string& filename) {
    std::lock_guard<std::mutex> pause_monitor(download_mutex);

    // Check socket status first
    if (!check_socket_status()) {
        DEBUG_PRINTF("ERROR: Socket is in invalid state. Cannot send download command.\n");
        return false;
    }

    // Prepare destination path
    std::string destPath = fs::current_path().string() + "/" + filename;  // Download to project root directory

    // Send download command
    packet cmd;
    memset(&cmd, 0, sizeof(packet)); // Clear the packet
    cmd.type = CMD_DOWNLOAD;
    cmd.seqn = get_next_seq();
    cmd.total_size = 0;
    strcpy(cmd.payload, filename.c_str());
    cmd.length = filename.length();

    DEBUG_PRINTF("DEBUG: Sending download command for file: %s with seq: %d\n", filename.c_str(), cmd.seqn);

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t bytes_sent = send(server_socket, &cmd, sizeof(packet), 0);
        if (bytes_sent <= 0) {
            DEBUG_PRINTF("ERROR: Failed to send download command: %s\n", strerror(errno));
            return false;
        }
        DEBUG_PRINTF("DEBUG: Sent %zd bytes (download command)\n", bytes_sent);
    }

    // Receive server response directly
    packet response;
    memset(&response, 0, sizeof(packet)); // Clear response packet

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        DEBUG_PRINTF("DEBUG: Waiting for download response...\n");
        ssize_t recv_bytes = recv(server_socket, &response, sizeof(packet), MSG_WAITALL);
        if (recv_bytes <= 0) {
            DEBUG_PRINTF("ERROR: Failed to receive download response: %s\n", strerror(errno));
            return false;
        }
        DEBUG_PRINTF("DEBUG: Received %zd bytes download response\n", recv_bytes);
    }

    DEBUG_PRINTF("DEBUG: Received download response: %s with seq: %d\n", response.payload, response.seqn);

    if (strcmp(response.payload, "OK") != 0) {
        printf("Erro ao baixar arquivo: %s\n", response.payload);
        return false;
    }

    // Allocate buffer for file
    size_t fileSize = response.total_size;
    char* fileData = new char[fileSize];
    size_t bytesRead = 0;

    DEBUG_PRINTF("DEBUG: Expecting file of size: %zu bytes\n", fileSize);

    // Receive file data packets directly
    while (bytesRead < fileSize) {
        packet dataPkt;
        memset(&dataPkt, 0, sizeof(packet));

        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            ssize_t recv_bytes = read_all(server_socket, &dataPkt, sizeof(packet));
            if (recv_bytes <= 0) {
                DEBUG_PRINTF("ERROR: Failed to receive file data: %s\n", strerror(errno));
                delete[] fileData;
                return false;
            }
            DEBUG_PRINTF("DEBUG: Received data packet with %zd bytes\n", recv_bytes);
        }

        if (dataPkt.type != DATA_PACKET) {
            DEBUG_PRINTF("ERROR: Unexpected packet type %d (expected DATA_PACKET)\n", dataPkt.type);
            delete[] fileData;
            return false;
        }

        memcpy(fileData + bytesRead, dataPkt.payload, dataPkt.length);
        bytesRead += dataPkt.length;

        DEBUG_PRINTF("DEBUG: Download progress: %zu/%zu bytes (%d%%)\n",
               bytesRead, fileSize, (int)(bytesRead * 100 / fileSize));
    }

    DEBUG_PRINTF("DEBUG: All file data received, saving to: %s\n", destPath.c_str());

    // Save file
    std::ofstream file(destPath, std::ios::binary);
    if (!file) {
        printf("Erro ao criar arquivo local '%s'.\n", destPath.c_str());
        delete[] fileData;
        return false;
    }

    file.write(fileData, fileSize);
    file.close();
    delete[] fileData;

    printf("Arquivo '%s' baixado com sucesso.\n", filename.c_str());
    return true;
}

bool delete_file(const std::string& filename) {
    // Check socket status first
    if (!check_socket_status()) {
        DEBUG_PRINTF("ERROR: Socket is in invalid state. Attempting to reset connection...\n");
        if (!reset_socket_connection()) {
            DEBUG_PRINTF("ERROR: Failed to reset connection. Cannot delete file.\n");
            return false;
        }
    }

    // Track our operation with unique sequence number
    uint16_t delete_seq = get_next_seq();

    // Build delete command packet
    packet cmd;
    memset(&cmd, 0, sizeof(packet));
    cmd.type = CMD_DELETE;
    cmd.seqn = delete_seq;
    cmd.total_size = 0;
    strcpy(cmd.payload, filename.c_str());
    cmd.length = strlen(filename.c_str());

    DEBUG_PRINTF("DEBUG: [DELETE] Sending command for file: %s with seq: %d\n", filename.c_str(), delete_seq);

    // First check if we need to clear socket buffer
    int bytes_available = 0;
    if (ioctl(server_socket, FIONREAD, &bytes_available) == 0) {
        if (bytes_available > 0) {
            DEBUG_PRINTF("WARNING: [DELETE] Socket has %d bytes of pending data, protocol desync detected\n",
                   bytes_available);
            DEBUG_PRINTF("WARNING: [DELETE] Resetting connection to ensure clean state\n");
            if (!reset_socket_connection()) {
                DEBUG_PRINTF("ERROR: [DELETE] Failed to reset connection. Cannot delete file.\n");
                return false;
            }
        }
    }

    // Send command with exclusive lock
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t bytes_sent = send(server_socket, &cmd, sizeof(packet), 0);
        if (bytes_sent <= 0) {
            DEBUG_PRINTF("ERROR: [DELETE] Failed to send command: %s\n", strerror(errno));
            return false;
        }
        DEBUG_PRINTF("DEBUG: [DELETE] Sent %zd bytes (delete command)\n", bytes_sent);
    }

    // Receive response with shorter timeout
    packet response;
    memset(&response, 0, sizeof(packet));

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        DEBUG_PRINTF("DEBUG: [DELETE] Waiting for response...\n");

        // Set a shorter timeout for this operation
        struct timeval short_timeout;
        short_timeout.tv_sec = 5;
        short_timeout.tv_usec = 0;
        setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &short_timeout, sizeof(short_timeout));

        // Receive with timeout
        ssize_t recv_bytes = recv(server_socket, &response, sizeof(packet), MSG_WAITALL);

        // Reset timeout to default
        struct timeval default_timeout;
        default_timeout.tv_sec = 10;
        default_timeout.tv_usec = 0;
        setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &default_timeout, sizeof(default_timeout));

        if (recv_bytes <= 0) {
            DEBUG_PRINTF("ERROR: [DELETE] Failed to receive response: %s\n", strerror(errno));
            // Try to reset connection on timeout
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                DEBUG_PRINTF("WARNING: [DELETE] Response timeout, attempting to reset connection\n");
                reset_socket_connection();
            }
            return false;
        }
        DEBUG_PRINTF("DEBUG: [DELETE] Received %zd bytes response\n", recv_bytes);
    }

    // Validate response packet
    DEBUG_PRINTF("DEBUG: [DELETE] Response: type=%d, seq=%d, length=%d, payload='%s'\n",
           response.type, response.seqn, response.length, response.payload);

    // Strict validation of response
    if (response.type != CMD_DELETE) {
        DEBUG_PRINTF("ERROR: [DELETE] Invalid response type: %d (expected %d)\n",
               response.type, CMD_DELETE);
        DEBUG_PRINTF("WARNING: [DELETE] Protocol desync detected, resetting connection\n");
        reset_socket_connection();
        return false;
    }

    if (response.seqn != delete_seq) {
        DEBUG_PRINTF("ERROR: [DELETE] Sequence number mismatch: %d (expected %d)\n",
               response.seqn, delete_seq);
        DEBUG_PRINTF("WARNING: [DELETE] Protocol desync detected, resetting connection\n");
        reset_socket_connection();
        return false;
    }

    // Process response based on payload
    if (strcmp(response.payload, "OK") == 0) {
        printf("Arquivo '%s' deletado com sucesso.\n", filename.c_str());

        // Also remove from local sync directory if it exists
        std::string localPath = sync_dir_path + "/" + filename;
        if (fs::exists(localPath)) {
            try {
                fs::remove(localPath);
                DEBUG_PRINTF("DEBUG: [DELETE] Removed local file: %s\n", localPath.c_str());

                // Update mtimes
                file_mutex.lock();
                file_mtimes.erase(filename);
                file_mutex.unlock();
            } catch (const std::exception& e) {
                DEBUG_PRINTF("DEBUG: [DELETE] Error removing local file: %s\n", e.what());
            }
        }

        return true;
    } else if (strcmp(response.payload, "NOT_FOUND") == 0) {
        printf("Arquivo '%s' não encontrado no servidor.\n", filename.c_str());
        return false;
    } else {
        printf("Erro ao deletar arquivo: %s\n", response.payload);
        return false;
    }
}

// Helper function to check socket status
bool check_socket_status() {
    if (!connection_alive.load()) {
        DEBUG_PRINTF("ERROR: Connection to server lost.\n");
        return false;
    }

    int error = 0;
    socklen_t len = sizeof(error);
    int retval = getsockopt(server_socket, SOL_SOCKET, SO_ERROR, &error, &len);

    if (retval != 0) {
        DEBUG_PRINTF("ERROR: getsockopt failed with error: %d\n", retval);
        return false;
    }

    if (error != 0) {
        DEBUG_PRINTF("ERROR: Socket error: %d\n", error);
        return false;
    }

    return true;
}

void list_server_files() {
    // Check socket status
    if (!check_socket_status()) {
        DEBUG_PRINTF("ERROR: Socket is in invalid state before sending list_server command. Attempting reset...\n");
        if (!reset_socket_connection()) {
            DEBUG_PRINTF("ERROR: Failed to reset connection. Cannot list server files.\n");
            return;
        }
    }

    // Check for pending data that would indicate protocol desync
    int bytes_available = 0;
    if (ioctl(server_socket, FIONREAD, &bytes_available) == 0 && bytes_available > 0) {
        DEBUG_PRINTF("WARNING: [LIST] Socket has %d bytes of pending data, protocol desync detected\n",
               bytes_available);
        DEBUG_PRINTF("WARNING: [LIST] Resetting connection to ensure clean state\n");
        if (!reset_socket_connection()) {
            DEBUG_PRINTF("ERROR: [LIST] Failed to reset connection. Cannot list server files.\n");
            return;
        }
    }

    // Send list_server command
    packet cmd;
    memset(&cmd, 0, sizeof(packet));
    cmd.type = CMD_LIST_SERVER;
    cmd.seqn = get_next_seq();
    cmd.total_size = 0;
    cmd.length = 0;

    DEBUG_PRINTF("DEBUG: [LIST] Sending list_server command with seq: %d\n", cmd.seqn);
    uint16_t expected_seq = cmd.seqn; // Store for validation

    // Send with exclusive lock
    ssize_t bytes_sent;
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        bytes_sent = write_all(server_socket, &cmd, sizeof(packet));
        if (bytes_sent <= 0) {
            DEBUG_PRINTF("ERROR: [LIST] Failed to send command: %s\n", strerror(errno));
            return;
        }
        DEBUG_PRINTF("DEBUG: [LIST] Sent %zd bytes (list_server command)\n", bytes_sent);
    }

    // Set a shorter timeout just for this operation
    struct timeval short_timeout;
    short_timeout.tv_sec = 5;
    short_timeout.tv_usec = 0;

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &short_timeout, sizeof(short_timeout));
    }

    // Receive server response
    packet response;
    memset(&response, 0, sizeof(packet));

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        DEBUG_PRINTF("DEBUG: [LIST] Waiting for response...\n");
        ssize_t recv_bytes = read_all(server_socket, &response, sizeof(packet));

        // Reset timeout to default
        struct timeval default_timeout;
        default_timeout.tv_sec = 10;
        default_timeout.tv_usec = 0;
        setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &default_timeout, sizeof(default_timeout));

        if (recv_bytes <= 0) {
            DEBUG_PRINTF("ERROR: [LIST] Failed to receive response: %s\n", strerror(errno));
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                DEBUG_PRINTF("WARNING: [LIST] Response timeout, resetting connection\n");
                reset_socket_connection();
            }
            return;
        }
        DEBUG_PRINTF("DEBUG: [LIST] Received %zd bytes response\n", recv_bytes);
    }

    // Validate response packet
    DEBUG_PRINTF("DEBUG: [LIST] Response: type=%d, seq=%d, length=%d, total_size=%u\n",
           response.type, response.seqn, response.length, response.total_size);

    // Strict validation
    if (response.type != CMD_LIST_SERVER) {
        DEBUG_PRINTF("ERROR: [LIST] Invalid response type: %d (expected %d)\n",
               response.type, CMD_LIST_SERVER);
        DEBUG_PRINTF("WARNING: [LIST] Protocol desync detected, resetting connection\n");
        reset_socket_connection();
        return;
    }

    if (response.seqn != expected_seq) {
        DEBUG_PRINTF("ERROR: [LIST] Sequence number mismatch: %d (expected %d)\n",
               response.seqn, expected_seq);
        DEBUG_PRINTF("WARNING: [LIST] Protocol desync detected, resetting connection\n");
        reset_socket_connection();
        return;
    }

    // Start with the response payload
    std::string fileList(response.payload, response.length);
    size_t expectedSize = response.total_size;

    DEBUG_PRINTF("DEBUG: [LIST] Initial response has %zu bytes of data, expecting %zu total bytes\n",
           fileList.length(), expectedSize);

    // If we expect more data (data packets)
    if (expectedSize > 0 && fileList.length() < expectedSize) {
        // Receive additional data packets directly
        while (fileList.length() < expectedSize) {
            packet dataPkt;
            memset(&dataPkt, 0, sizeof(packet));

            {
                std::lock_guard<std::mutex> lock(socket_mutex);
                ssize_t recv_bytes = read_all(server_socket, &dataPkt, sizeof(packet));
                if (recv_bytes <= 0) {
                    DEBUG_PRINTF("ERROR: [LIST] Failed to receive additional data: %s\n", strerror(errno));
                    break;
                }
                DEBUG_PRINTF("DEBUG: [LIST] Received additional data packet with %zd bytes\n", recv_bytes);
            }

            if (dataPkt.type != DATA_PACKET) {
                DEBUG_PRINTF("ERROR: [LIST] Unexpected packet type %d (expected DATA_PACKET)\n", dataPkt.type);
                DEBUG_PRINTF("WARNING: [LIST] Protocol desync detected, resetting connection\n");
                reset_socket_connection();
                return;
            }

            fileList.append(dataPkt.payload, dataPkt.length);
            DEBUG_PRINTF("DEBUG: [LIST] Received additional data, now have %zu/%zu bytes\n",
                   fileList.length(), expectedSize);
        }
    }

    // Print file list
    if (fileList.empty()) {
        printf("Nenhum arquivo no servidor.\n");
        return;
    }

    printf("Arquivos no servidor:\n");
    printf("%-30s %-10s %-20s %-20s %-20s\n", "Nome", "Tamanho", "Modificado", "Acessado", "Criado");

    std::istringstream stream(fileList);
    std::string line;
    int file_count = 0;

    while (std::getline(stream, line)) {
        std::istringstream lineStream(line);
        std::string filename, sizeStr, mtimeStr, atimeStr, ctimeStr;

        std::getline(lineStream, filename, ',');
        std::getline(lineStream, sizeStr, ',');
        std::getline(lineStream, mtimeStr, ',');
        std::getline(lineStream, atimeStr, ',');
        std::getline(lineStream, ctimeStr, ',');

        if (!filename.empty() && !sizeStr.empty()) {
            // Validate time strings before conversion to avoid exceptions
            if (mtimeStr.empty() || atimeStr.empty() || ctimeStr.empty()) {
                DEBUG_PRINTF("WARNING: [LIST] Skipping malformed line: %s\n", line.c_str());
                continue;
            }

            time_t mtime = 0, atime = 0, ctime = 0;
            try {
                mtime = std::stol(mtimeStr);
                atime = std::stol(atimeStr);
                ctime = std::stol(ctimeStr);
            } catch (const std::exception& e) {
                DEBUG_PRINTF("WARNING: [LIST] Invalid time values in line: %s (%s)\n", line.c_str(), e.what());
                continue;
            }

            char mtimeStr[64], atimeStr[64], ctimeStr[64];
            strftime(mtimeStr, sizeof(mtimeStr), "%Y-%m-%d %H:%M:%S", localtime(&mtime));
            strftime(atimeStr, sizeof(atimeStr), "%Y-%m-%d %H:%M:%S", localtime(&atime));
            strftime(ctimeStr, sizeof(ctimeStr), "%Y-%m-%d %H:%M:%S", localtime(&ctime));

            printf("%-30s %-10s %-20s %-20s %-20s\n",
                   filename.c_str(),
                   sizeStr.c_str(),
                   mtimeStr,
                   atimeStr,
                   ctimeStr);
            file_count++;
        }
    }

    DEBUG_PRINTF("DEBUG: [LIST] Successfully displayed %d files from server\n", file_count);
}

void list_client_files() {
    if (!sync_dir_exists()) {
        printf("Diretório de sincronização não existe.\n");
        return;
    }

    printf("Arquivos locais em %s:\n", sync_dir_path.c_str());
    printf("Nome\t\tTamanho\t\tModificado em\t\tCriado em\n");

    for (const auto& entry : fs::directory_iterator(sync_dir_path)) {
        if (entry.is_regular_file()) {
            struct stat st;
            std::string filepath = entry.path().string();

            if (stat(filepath.c_str(), &st) == 0) {
                char mtime_str[30], ctime_str[30];
                strftime(mtime_str, sizeof(mtime_str), "%Y-%m-%d %H:%M:%S", localtime(&st.st_mtime));
                strftime(ctime_str, sizeof(ctime_str), "%Y-%m-%d %H:%M:%S", localtime(&st.st_ctime));

                printf("%s\t\t%lld bytes\t%s\t%s\n",
                       entry.path().filename().c_str(), (long long)st.st_size, mtime_str, ctime_str);
            }
        }
    }
}

void get_sync_dir() {
    // Check socket status first
    if (!check_socket_status()) {
        DEBUG_PRINTF("ERROR: Socket is in invalid state. Cannot send get_sync_dir command.\n");
        return;
    }

    // Create sync directory if it doesn't exist
    watch_sync_dir();

    // Send get_sync_dir command
    packet cmd;
    memset(&cmd, 0, sizeof(packet)); // Clear packet
    cmd.type = CMD_GET_SYNC_DIR;
    cmd.seqn = get_next_seq();
    cmd.total_size = 0;
    cmd.length = 0;

    DEBUG_PRINTF("DEBUG: Sending get_sync_dir command with seq: %d\n", cmd.seqn);

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t bytes_sent = send(server_socket, &cmd, sizeof(packet), 0);
        if (bytes_sent <= 0) {
            DEBUG_PRINTF("Erro ao enviar comando get_sync_dir: %s\n", strerror(errno));
            return;
        }
        DEBUG_PRINTF("DEBUG: Sent %zd bytes (get_sync_dir command)\n", bytes_sent);
    }

    // Wait for server response using direct receive
    DEBUG_PRINTF("DEBUG: Waiting for get_sync_dir response...\n");
    packet response;
    memset(&response, 0, sizeof(packet)); // Clear response packet

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t recv_bytes = recv(server_socket, &response, sizeof(packet), MSG_WAITALL);
        if (recv_bytes <= 0) {
            DEBUG_PRINTF("Erro ao receber resposta do servidor: %s\n", strerror(errno));
            return;
        }
        DEBUG_PRINTF("DEBUG: Received %zd bytes for get_sync_dir response\n", recv_bytes);
    }

    DEBUG_PRINTF("DEBUG: Received get_sync_dir response: %s with seq: %d, total_size: %u\n",
           response.payload, response.seqn, response.total_size);

    if (response.seqn != cmd.seqn) {
        DEBUG_PRINTF("WARNING: Sequence number mismatch: got %d, expected %d\n", response.seqn, cmd.seqn);
    }

    if (strcmp(response.payload, "OK") != 0) {
        printf("Erro ao inicializar diretório de sincronização: %s\n", response.payload);
        return;
    }

    // Number of files to receive
    size_t numFiles = response.total_size;
    printf("Inicializando diretório de sincronização com %zu arquivos...\n", numFiles);

    if (numFiles == 0) {
        printf("Diretório de sincronização inicializado (vazio).\n");
        update_file_mtimes();
        return;
    }

    // For each file notification, receive it directly
    for (size_t i = 0; i < numFiles; i++) {
        packet filePkt;
        memset(&filePkt, 0, sizeof(packet));

        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            ssize_t recv_bytes = recv(server_socket, &filePkt, sizeof(packet), MSG_WAITALL);
            if (recv_bytes <= 0) {
                DEBUG_PRINTF("Erro ao receber notificação de arquivo: %s\n", strerror(errno));
                return;
            }
            DEBUG_PRINTF("DEBUG: Received file notification %zu/%zu: %s (%zd bytes)\n",
                   i+1, numFiles, filePkt.payload, recv_bytes);
        }

        if (filePkt.type != SYNC_NOTIFICATION) {
            DEBUG_PRINTF("ERRO: Esperava notificação de arquivo, recebeu pacote tipo %d\n", filePkt.type);
            continue;
        }

        // Process this file notification
        handle_server_notification(filePkt);
    }

    DEBUG_PRINTF("Diretório de sincronização inicializado com %zu arquivos.\n", numFiles);
    update_file_mtimes();
}

// Add this function to reset the socket connection
bool reset_socket_connection() {
    // Close existing socket
    if (server_socket != -1) {
        close(server_socket);
        DEBUG_PRINTF("DEBUG: Socket connection reset - closed old socket\n");
    }

    // Create new socket
    server_socket = create_socket();
    if (server_socket == -1) {
        DEBUG_PRINTF("ERROR: Failed to create new socket\n");
        return false;
    }

    // Set socket to blocking mode
    int flags = fcntl(server_socket, F_GETFL, 0);
    fcntl(server_socket, F_SETFL, flags & ~O_NONBLOCK);

    // Set socket timeout options
    struct timeval timeout;
    timeout.tv_sec = 10;  // Reduce timeout from 30 to 10 seconds
    timeout.tv_usec = 0;

    // Set send and receive timeout
    if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("Error setting receive timeout");
    }
    if (setsockopt(server_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("Error setting send timeout");
    }

    // Set TCP_NODELAY to disable Nagle's algorithm
    int flag = 1;
    if (setsockopt(server_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        perror("Error setting TCP_NODELAY");
    }

    // Connect to server
    if (connect_socket(server_socket, g_server_ip.c_str(), g_server_port) < 0) {
        DEBUG_PRINTF("ERROR: Failed to reconnect to server at %s:%d\n", g_server_ip.c_str(), g_server_port);
        close(server_socket);
        server_socket = -1;
        return false;
    }

    DEBUG_PRINTF("DEBUG: Successfully reconnected to server %s:%d\n", g_server_ip.c_str(), g_server_port);

    // Re-login user
    packet login_pkt;
    memset(&login_pkt, 0, sizeof(packet));
    login_pkt.type = CMD_LOGIN;
    login_pkt.seqn = get_next_seq();
    login_pkt.total_size = 0;
    strcpy(login_pkt.payload, current_username.c_str());
    login_pkt.length = strlen(current_username.c_str());

    ssize_t bytes_sent = send(server_socket, &login_pkt, sizeof(packet), 0);
    if (bytes_sent <= 0) {
        DEBUG_PRINTF("ERROR: Failed to send login packet after reconnection\n");
        close(server_socket);
        server_socket = -1;
        return false;
    }

    // Receive login response
    packet response;
    memset(&response, 0, sizeof(packet));
    ssize_t bytes_received = recv(server_socket, &response, sizeof(packet), MSG_WAITALL);
    if (bytes_received <= 0 || response.type != CMD_LOGIN) {
        DEBUG_PRINTF("ERROR: Failed to receive valid login response after reconnection\n");
        close(server_socket);
        server_socket = -1;
        return false;
    }

    DEBUG_PRINTF("DEBUG: Successfully re-authenticated to server\n");
    connection_alive.store(true);
    return true;
}
