#include "../headers/sync.h"
#include "../../common/headers/socket_utils.h"
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

namespace fs = std::filesystem;

// Global variables
int server_socket = -1;
std::string sync_dir_path;
std::string current_username;

// Mutex for file operations
std::mutex file_mutex;

// Mutex for socket communication
std::mutex socket_mutex;

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

void sync_start(const char* username, const char* server_ip, int port) {
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
    
    if (connect_socket(server_socket, server_ip, port) < 0) {
        perror("Erro ao conectar ao servidor");
        return;
    }
    
    printf("Conectado ao servidor %s:%d\n", server_ip, port);
    
    // Make sure sync directory exists before we start
    watch_sync_dir();
    
    // Start threads for monitoring BEFORE sending any commands
    // std::thread server_thread(monitor_server_notifications);
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
    
    printf("DEBUG: Sending login packet with seq: %d, type: %d, length: %d\n", 
           login_pkt.seqn, login_pkt.type, login_pkt.length);
    printf("DEBUG: Packet size: %zu bytes\n", sizeof(packet));
    
    // Use direct socket send for login packet
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t bytes_sent = send(server_socket, &login_pkt, sizeof(packet), 0);
        if (bytes_sent <= 0) {
            printf("ERROR: Failed to send login packet: %s\n", strerror(errno));
            close(server_socket);
            return;
        }
        printf("DEBUG: Sent %zd bytes directly via socket\n", bytes_sent);
    }
    
    // Wait for server response using direct socket operations
    packet response;
    memset(&response, 0, sizeof(packet));
    
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        printf("DEBUG: Waiting for login response...\n");
        ssize_t bytes_received = recv(server_socket, &response, sizeof(packet), MSG_WAITALL);
        if (bytes_received <= 0) {
            printf("ERROR: Failed to receive login response: %s\n", strerror(errno));
            close(server_socket);
            return;
        }
        printf("DEBUG: Received %zd bytes login response\n", bytes_received);
    }
    
    printf("DEBUG: Received login response with seq: %d, type: %d\n", response.seqn, response.type);
    
    if (response.type != CMD_LOGIN) {
        printf("Erro na resposta de login: tipo inesperado %d\n", response.type);
        close(server_socket);
        return;
    }
    
    // Initialize sync
    initialize_sync();
    
    // Detach threads to run in background
    // server_thread.detach();
    file_thread.detach();
    
    printf("Sincronização iniciada. Use os comandos para interagir.\n");
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
    
    printf("DEBUG Monitor: Thread starting\n");
    
    // Signal that the monitor thread is ready
    {
        std::lock_guard<std::mutex> lock(monitor_ready_mutex);
        monitor_thread_ready = true;
        monitor_ready_cv.notify_all();
    }
    
    printf("DEBUG Monitor: Thread ready - waiting for packets\n");
    
    while (true) {
        // Wait for notifications from the server
        packet pkt;
        memset(&pkt, 0, sizeof(packet)); // Clear the packet before reading
        
        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            size_t bytes_read = read_all(server_socket, &pkt, sizeof(packet));
            if (bytes_read != sizeof(packet)) {
                printf("Error reading from server: read %zu of %zu bytes (errno=%d: %s)\n", 
                       bytes_read, sizeof(packet), errno, strerror(errno));
                // TODO: Handle reconnection
                break;
            }
        }
        
        printf("DEBUG Monitor: Received packet type: %d, seq: %d, length: %d, payload: %.10s...\n", 
               pkt.type, pkt.seqn, pkt.length, pkt.payload);
        
        // Handle the packet based on its type
        if (pkt.type == SYNC_NOTIFICATION) {
            printf("DEBUG Monitor: Processing sync notification\n");
            
            // Check if this is part of a get_sync_dir response
            if (command_completed && files_remaining > 0) {
                files_remaining--;
                printf("DEBUG Monitor: Processing sync dir file (%zu remaining)\n", files_remaining);
                
                if (files_remaining == 0) {
                    // Signal that the get_sync_dir command is fully complete
                    command_completed = false;
                    printf("DEBUG Monitor: All sync dir files processed\n");
                }
                // Process the file notification regardless
                handle_server_notification(pkt);
            } else {
                // It's a standalone sync notification from another device
                printf("DEBUG Monitor: Processing notification from another device\n");
                handle_server_notification(pkt);
            }
        } else {
            // It's a response to a command
            printf("DEBUG Monitor: Received command response, adding to responses map\n");
            
            // Special handling for get_sync_dir
            if (pkt.type == CMD_GET_SYNC_DIR) {
                // Mark that we're expecting file notifications
                if (strcmp(pkt.payload, "OK") == 0) {
                    command_completed = true;
                    files_remaining = pkt.total_size;
                    expected_seqn = pkt.seqn;
                    printf("DEBUG Monitor: get_sync_dir response OK, expecting %zu files\n", files_remaining);
                    
                    // If no files to sync, mark command as done immediately
                    if (files_remaining == 0) {
                        command_completed = false;
                        printf("DEBUG Monitor: No files to sync, command completed\n");
                    }
                }
            }
            
            // Store the response in the map
            {
                std::lock_guard<std::mutex> lock(responses_mutex);
                responses[pkt.seqn] = pkt;
                printf("DEBUG Monitor: Added response for seq %d to map (map size: %zu)\n", 
                       pkt.seqn, responses.size());
                responses_cv.notify_all();
            }
        }
    }
}

void handle_server_notification(packet& pkt) {
    std::string filename(pkt.payload, pkt.length);
    
    if (pkt.seqn == 1) {
        // Delete notification
        std::string filepath = sync_dir_path + "/" + filename;
        
        file_mutex.lock();
        if (file_mtimes.find(filename) != file_mtimes.end()) {
            file_mtimes.erase(filename);
            fs::remove(filepath);
            printf("Arquivo '%s' removido por outro dispositivo.\n", filename.c_str());
        }
        file_mutex.unlock();
    } else {
        // New/updated file notification
        download_file(filename);
        printf("Arquivo '%s' atualizado por outro dispositivo.\n", filename.c_str());
    }
}

bool upload_file(const std::string& filepath) {
    // Check socket status first
    if (!check_socket_status()) {
        printf("ERROR: Socket is in invalid state. Cannot send upload command.\n");
        return false;
    }
    
    // Get just the filename from the path
    std::string filename = fs::path(filepath).filename().string();
    
    printf("DEBUG: Attempting to upload file: %s\n", filepath.c_str());
    
    // Check if file exists
    if (!fs::exists(filepath)) {
        printf("Arquivo '%s' não encontrado.\n", filepath.c_str());
        return false;
    }
    
    printf("DEBUG: File exists, opening...\n");
    
    // Open the file
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        printf("Erro ao abrir o arquivo '%s'.\n", filepath.c_str());
        return false;
    }
    
    // Get file size
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    printf("DEBUG: File size: %zu bytes\n", fileSize);
    
    // Read file content
    char* fileData = new char[fileSize];
    file.read(fileData, fileSize);
    file.close();
    
    printf("DEBUG: File read into memory\n");
    
    // Send upload command
    packet cmd;
    memset(&cmd, 0, sizeof(packet)); // Clear packet
    cmd.type = CMD_UPLOAD;
    cmd.seqn = get_next_seq();
    cmd.total_size = fileSize;
    strcpy(cmd.payload, filename.c_str());
    cmd.length = filename.length();
    
    printf("DEBUG: Sending upload command packet for file: %s, size: %zu\n", 
           filename.c_str(), fileSize);
    
    // Send the command
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t bytes_sent = send(server_socket, &cmd, sizeof(packet), 0);
        if (bytes_sent <= 0) {
            printf("ERROR: Failed to send upload command: %s\n", strerror(errno));
            delete[] fileData;
            return false;
        }
        printf("DEBUG: Sent %zd bytes (upload command)\n", bytes_sent);
    }
    
    printf("DEBUG: Upload command sent, sending file data...\n");
    
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
        
        printf("DEBUG: Sending data packet %d, bytes: %zu\n", dataPkt.seqn, bytesToSend);
        
        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            ssize_t bytes_sent = send(server_socket, &dataPkt, sizeof(packet), 0);
            if (bytes_sent <= 0) {
                printf("ERROR: Failed to send file data: %s\n", strerror(errno));
                delete[] fileData;
                return false;
            }
            printf("DEBUG: Sent %zd bytes (data packet)\n", bytes_sent);
        }
        
        bytesSent += bytesToSend;
        printf("DEBUG: Progress: %zu/%zu bytes sent (%d%%)\n", 
               bytesSent, fileSize, (int)(bytesSent * 100 / fileSize));
    }
    
    delete[] fileData;
    
    printf("DEBUG: All file data sent, waiting for server response...\n");
    
    // Receive server response directly
    packet response;
    memset(&response, 0, sizeof(packet)); // Clear response packet
    
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        printf("DEBUG: Waiting for upload response...\n");
        ssize_t recv_bytes = recv(server_socket, &response, sizeof(packet), MSG_WAITALL);
        if (recv_bytes <= 0) {
            printf("ERROR: Failed to receive upload response: %s\n", strerror(errno));
            return false;
        }
        printf("DEBUG: Received %zd bytes upload response\n", recv_bytes);
    }
    
    printf("DEBUG: Received upload response: %s\n", response.payload);
    
    if (strcmp(response.payload, "OK") == 0) {
        printf("Arquivo '%s' enviado com sucesso.\n", filename.c_str());
        
        // Check if file was copied to sync directory
        std::string syncPath = sync_dir_path + "/" + filename;
        printf("DEBUG: Checking if file was copied to sync directory: %s\n", syncPath.c_str());
        
        if (fs::exists(syncPath)) {
            printf("DEBUG: File exists in sync directory\n");
        } else {
            printf("DEBUG: File does NOT exist in sync directory\n");
            
            // Try to copy the file to sync directory if it's not there
            try {
                fs::copy_file(filepath, syncPath, fs::copy_options::overwrite_existing);
                printf("DEBUG: Manually copied file to sync directory\n");
            } catch (const std::exception& e) {
                printf("DEBUG: Error copying file to sync directory: %s\n", e.what());
            }
        }
        
        return true;
    } else {
        printf("Erro ao enviar arquivo: %s\n", response.payload);
        return false;
    }
}

bool download_file(const std::string& filename) {
    // Check socket status first
    if (!check_socket_status()) {
        printf("ERROR: Socket is in invalid state. Cannot send download command.\n");
        return false;
    }
    
    // Prepare destination path
    std::string destPath = sync_dir_path + "/" + filename;  // Download to sync directory
    
    // Send download command
    packet cmd;
    memset(&cmd, 0, sizeof(packet)); // Clear the packet
    cmd.type = CMD_DOWNLOAD;
    cmd.seqn = get_next_seq();
    cmd.total_size = 0;
    strcpy(cmd.payload, filename.c_str());
    cmd.length = filename.length();
    
    printf("DEBUG: Sending download command for file: %s with seq: %d\n", filename.c_str(), cmd.seqn);
    
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t bytes_sent = send(server_socket, &cmd, sizeof(packet), 0);
        if (bytes_sent <= 0) {
            printf("ERROR: Failed to send download command: %s\n", strerror(errno));
            return false;
        }
        printf("DEBUG: Sent %zd bytes (download command)\n", bytes_sent);
    }
    
    // Receive server response directly
    packet response;
    memset(&response, 0, sizeof(packet)); // Clear response packet
    
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        printf("DEBUG: Waiting for download response...\n");
        ssize_t recv_bytes = recv(server_socket, &response, sizeof(packet), MSG_WAITALL);
        if (recv_bytes <= 0) {
            printf("ERROR: Failed to receive download response: %s\n", strerror(errno));
            return false;
        }
        printf("DEBUG: Received %zd bytes download response\n", recv_bytes);
    }
    
    printf("DEBUG: Received download response: %s with seq: %d\n", response.payload, response.seqn);
    
    if (strcmp(response.payload, "OK") != 0) {
        printf("Erro ao baixar arquivo: %s\n", response.payload);
        return false;
    }
    
    // Allocate buffer for file
    size_t fileSize = response.total_size;
    char* fileData = new char[fileSize];
    size_t bytesRead = 0;
    
    printf("DEBUG: Expecting file of size: %zu bytes\n", fileSize);
    
    // Receive file data packets directly
    while (bytesRead < fileSize) {
        packet dataPkt;
        memset(&dataPkt, 0, sizeof(packet));
        
        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            ssize_t recv_bytes = recv(server_socket, &dataPkt, sizeof(packet), MSG_WAITALL);
            if (recv_bytes <= 0) {
                printf("ERROR: Failed to receive file data: %s\n", strerror(errno));
                delete[] fileData;
                return false;
            }
            printf("DEBUG: Received data packet with %zd bytes\n", recv_bytes);
        }
        
        if (dataPkt.type != DATA_PACKET) {
            printf("ERROR: Unexpected packet type %d (expected DATA_PACKET)\n", dataPkt.type);
            delete[] fileData;
            return false;
        }
        
        memcpy(fileData + bytesRead, dataPkt.payload, dataPkt.length);
        bytesRead += dataPkt.length;
        
        printf("DEBUG: Download progress: %zu/%zu bytes (%d%%)\n", 
               bytesRead, fileSize, (int)(bytesRead * 100 / fileSize));
    }
    
    printf("DEBUG: All file data received, saving to: %s\n", destPath.c_str());
    
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
        printf("ERROR: Socket is in invalid state. Cannot send delete command.\n");
        return false;
    }
    
    // Send delete command
    packet cmd;
    memset(&cmd, 0, sizeof(packet)); // Clear the packet
    cmd.type = CMD_DELETE;
    cmd.seqn = get_next_seq();
    cmd.total_size = 0;
    strcpy(cmd.payload, filename.c_str());
    cmd.length = filename.length();
    
    printf("DEBUG: Sending delete command for file: %s\n", filename.c_str());
    
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t bytes_sent = send(server_socket, &cmd, sizeof(packet), 0);
        if (bytes_sent <= 0) {
            printf("ERROR: Failed to send delete command: %s\n", strerror(errno));
            return false;
        }
        printf("DEBUG: Sent %zd bytes (delete command)\n", bytes_sent);
    }
    
    // Receive server response directly
    packet response;
    memset(&response, 0, sizeof(packet)); // Clear response packet
    
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        printf("DEBUG: Waiting for delete response...\n");
        ssize_t recv_bytes = recv(server_socket, &response, sizeof(packet), MSG_WAITALL);
        if (recv_bytes <= 0) {
            printf("ERROR: Failed to receive delete response: %s\n", strerror(errno));
            return false;
        }
        printf("DEBUG: Received %zd bytes delete response\n", recv_bytes);
    }
    
    printf("DEBUG: Received server response: %s\n", response.payload);
    
    if (strcmp(response.payload, "OK") == 0) {
        printf("Arquivo '%s' deletado com sucesso.\n", filename.c_str());
        
        // Also remove from local sync directory if it exists
        std::string localPath = sync_dir_path + "/" + filename;
        if (fs::exists(localPath)) {
            try {
                fs::remove(localPath);
                printf("DEBUG: Deleted local file: %s\n", localPath.c_str());
                
                // Update mtimes
                file_mutex.lock();
                file_mtimes.erase(filename);
                file_mutex.unlock();
            } catch (const std::exception& e) {
                printf("DEBUG: Error deleting local file: %s\n", e.what());
            }
        }
        
        return true;
    } else {
        printf("Erro ao deletar arquivo: %s\n", response.payload);
        return false;
    }
}

// Helper function to check socket status
bool check_socket_status() {
    int error = 0;
    socklen_t len = sizeof(error);
    int retval = getsockopt(server_socket, SOL_SOCKET, SO_ERROR, &error, &len);
    
    if (retval != 0) {
        printf("ERROR: getsockopt failed with error: %d\n", retval);
        return false;
    }
    
    if (error != 0) {
        printf("ERROR: Socket error: %d\n", error);
        return false;
    }
    
    return true;
}

void list_server_files() {
    // Check socket status
    if (!check_socket_status()) {
        printf("ERROR: Socket is in invalid state before sending list_server command.\n");
        return;
    }
    
    // Print socket options for debugging
    struct timeval timeout;
    socklen_t len = sizeof(timeout);
    if (getsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, &len) == 0) {
        printf("DEBUG: Current socket receive timeout: %ld seconds, %d microseconds\n", 
               timeout.tv_sec, timeout.tv_usec);
    }
    
    // Send list_server command
    packet cmd;
    memset(&cmd, 0, sizeof(packet)); // Clear the packet
    cmd.type = CMD_LIST_SERVER;
    cmd.seqn = get_next_seq();
    cmd.total_size = 0;
    cmd.length = 0;
    
    printf("DEBUG: Sending list_server command with seq: %d\n", cmd.seqn);
    
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t bytes_sent = send(server_socket, &cmd, sizeof(packet), 0);
        if (bytes_sent <= 0) {
            printf("ERROR: Failed to send list_server command: %s\n", strerror(errno));
            return;
        }
        printf("DEBUG: Sent %zd bytes (list_server command)\n", bytes_sent);
    }
    
    // Check socket status after sending
    if (!check_socket_status()) {
        printf("ERROR: Socket became invalid after sending list_server command.\n");
        return;
    }
    
    // Receive server response directly
    packet response;
    memset(&response, 0, sizeof(packet)); // Clear response packet
    
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        printf("DEBUG: Waiting for list_server response...\n");
        // Wait with a specific timeout
        ssize_t recv_bytes = recv(server_socket, &response, sizeof(packet), MSG_WAITALL);
        if (recv_bytes <= 0) {
            printf("ERROR: Failed to receive list_server response: %s (errno=%d)\n", 
                   strerror(errno), errno);
            return;
        }
        printf("DEBUG: Received %zd bytes list_server response\n", recv_bytes);
    }
    
    // Check socket status after receiving
    if (!check_socket_status()) {
        printf("ERROR: Socket became invalid after receiving list_server response.\n");
    } else {
        printf("DEBUG: Socket is still valid after list_server command.\n");
    }
    
    // Start with the response payload
    std::string fileList(response.payload);
    size_t expectedSize = response.total_size;
    
    printf("DEBUG: Received initial list_server response with %zu bytes of data, expecting %zu total bytes\n", 
           fileList.length(), expectedSize);
    
    // If we expect more data (data packets)
    if (fileList.length() < expectedSize) {
        // Receive additional data packets directly
        while (fileList.length() < expectedSize) {
            packet dataPkt;
            memset(&dataPkt, 0, sizeof(packet));
            
            {
                std::lock_guard<std::mutex> lock(socket_mutex);
                ssize_t recv_bytes = recv(server_socket, &dataPkt, sizeof(packet), MSG_WAITALL);
                if (recv_bytes <= 0) {
                    printf("ERROR: Failed to receive additional data: %s\n", strerror(errno));
                    break;
                }
                printf("DEBUG: Received additional data packet with %zd bytes\n", recv_bytes);
            }
            
            if (dataPkt.type != DATA_PACKET) {
                printf("ERROR: Unexpected packet type %d (expected DATA_PACKET)\n", dataPkt.type);
                continue;
            }
            
            fileList += dataPkt.payload;
            printf("DEBUG: Received additional list_server data, now have %zu/%zu bytes\n", 
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
    
    while (std::getline(stream, line)) {
        std::istringstream lineStream(line);
        std::string filename, sizeStr, mtimeStr, atimeStr, ctimeStr;
        
        std::getline(lineStream, filename, ',');
        std::getline(lineStream, sizeStr, ',');
        std::getline(lineStream, mtimeStr, ',');
        std::getline(lineStream, atimeStr, ',');
        std::getline(lineStream, ctimeStr, ',');
        
        if (!filename.empty() && !sizeStr.empty()) {
            time_t mtime = std::stol(mtimeStr);
            time_t atime = std::stol(atimeStr);
            time_t ctime = std::stol(ctimeStr);
            
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
        }
    }
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
        printf("ERROR: Socket is in invalid state. Cannot send get_sync_dir command.\n");
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
    
    printf("DEBUG: Sending get_sync_dir command with seq: %d\n", cmd.seqn);
    
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t bytes_sent = send(server_socket, &cmd, sizeof(packet), 0);
        if (bytes_sent <= 0) {
            printf("Erro ao enviar comando get_sync_dir: %s\n", strerror(errno));
            return;
        }
        printf("DEBUG: Sent %zd bytes (get_sync_dir command)\n", bytes_sent);
    }
    
    // Wait for server response using direct receive
    printf("DEBUG: Waiting for get_sync_dir response...\n");
    packet response;
    memset(&response, 0, sizeof(packet)); // Clear response packet
    
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        ssize_t recv_bytes = recv(server_socket, &response, sizeof(packet), MSG_WAITALL);
        if (recv_bytes <= 0) {
            printf("Erro ao receber resposta do servidor: %s\n", strerror(errno));
            return;
        }
        printf("DEBUG: Received %zd bytes for get_sync_dir response\n", recv_bytes);
    }
    
    printf("DEBUG: Received get_sync_dir response: %s with seq: %d, total_size: %u\n", 
           response.payload, response.seqn, response.total_size);
    
    if (response.seqn != cmd.seqn) {
        printf("WARNING: Sequence number mismatch: got %d, expected %d\n", response.seqn, cmd.seqn);
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
                printf("Erro ao receber notificação de arquivo: %s\n", strerror(errno));
                return;
            }
            printf("DEBUG: Received file notification %zu/%zu: %s (%zd bytes)\n", 
                   i+1, numFiles, filePkt.payload, recv_bytes);
        }
        
        if (filePkt.type != SYNC_NOTIFICATION) {
            printf("ERRO: Esperava notificação de arquivo, recebeu pacote tipo %d\n", filePkt.type);
            continue;
        }
        
        // Process this file notification
        handle_server_notification(filePkt);
    }
    
    printf("Diretório de sincronização inicializado com %zu arquivos.\n", numFiles);
    update_file_mtimes();
}
