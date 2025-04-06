#include "sync.h"
#include "../common/socket_utils.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace fs = std::filesystem;

// Global variables
int server_socket = -1;
std::string sync_dir_path;
std::string current_username;

// Mutex for file operations
std::mutex file_mutex;

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
    
    // Connect to server
    server_socket = create_socket();
    if (connect_socket(server_socket, server_ip, port) < 0) {
        perror("Erro ao conectar ao servidor");
        return;
    }
    
    printf("Conectado ao servidor %s:%d\n", server_ip, port);
    
    // Send login packet
    packet login_pkt;
    login_pkt.type = CMD_LOGIN;
    login_pkt.seqn = 0;
    login_pkt.total_size = 0;
    strcpy(login_pkt.payload, username);
    login_pkt.length = strlen(username);
    
    if (write(server_socket, &login_pkt, sizeof(packet)) <= 0) {
        perror("Erro ao enviar login");
        close(server_socket);
        return;
    }
    
    // Wait for server response
    if (read(server_socket, &login_pkt, sizeof(packet)) <= 0 || login_pkt.type != CMD_LOGIN) {
        perror("Erro na resposta de login");
        close(server_socket);
        return;
    }
    
    // Initialize sync directory
    initialize_sync();
    
    // Start threads for monitoring
    std::thread server_thread(monitor_server_notifications);
    std::thread file_thread(check_for_file_changes);
    
    // Detach threads to run in background
    server_thread.detach();
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

void monitor_server_notifications() {
    packet pkt;
    
    while (true) {
        if (read(server_socket, &pkt, sizeof(packet)) <= 0) {
            // Connection lost
            printf("Conexão com o servidor perdida.\n");
            close(server_socket);
            return;
        }
        
        if (pkt.type == SYNC_NOTIFICATION) {
            // Process notification from server
            handle_server_notification(pkt);
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
    cmd.type = CMD_UPLOAD;
    cmd.seqn = 0;
    cmd.total_size = fileSize;
    strcpy(cmd.payload, filename.c_str());
    cmd.length = filename.length();
    
    printf("DEBUG: Sending upload command packet for file: %s, size: %zu\n", 
           filename.c_str(), fileSize);
    
    if (write(server_socket, &cmd, sizeof(packet)) <= 0) {
        printf("Erro ao enviar comando de upload.\n");
        delete[] fileData;
        return false;
    }
    
    printf("DEBUG: Upload command sent, sending file data...\n");
    
    // Send file data in chunks
    size_t bytesSent = 0;
    while (bytesSent < fileSize) {
        packet dataPkt;
        dataPkt.type = DATA_PACKET;
        dataPkt.seqn = bytesSent / sizeof(dataPkt.payload) + 1;
        
        size_t bytesToSend = std::min(sizeof(dataPkt.payload), fileSize - bytesSent);
        memcpy(dataPkt.payload, fileData + bytesSent, bytesToSend);
        dataPkt.length = bytesToSend;
        
        printf("DEBUG: Sending data packet %d, bytes: %zu\n", dataPkt.seqn, bytesToSend);
        
        if (write(server_socket, &dataPkt, sizeof(packet)) <= 0) {
            printf("Erro ao enviar dados do arquivo.\n");
            delete[] fileData;
            return false;
        }
        
        bytesSent += bytesToSend;
        printf("DEBUG: Progress: %zu/%zu bytes sent (%d%%)\n", 
               bytesSent, fileSize, (int)(bytesSent * 100 / fileSize));
    }
    
    delete[] fileData;
    
    printf("DEBUG: All file data sent, waiting for server response...\n");
    
    // Get server response
    packet response;
    if (read(server_socket, &response, sizeof(packet)) <= 0) {
        printf("Erro ao receber resposta do servidor.\n");
        return false;
    }
    
    printf("DEBUG: Received server response: %s\n", response.payload);
    
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
    // Prepare destination path
    std::string destPath = sync_dir_path + "/" + filename;
    
    // Send download command
    packet cmd;
    cmd.type = CMD_DOWNLOAD;
    cmd.seqn = 0;
    cmd.total_size = 0;
    strcpy(cmd.payload, filename.c_str());
    cmd.length = filename.length();
    
    if (write(server_socket, &cmd, sizeof(packet)) <= 0) {
        printf("Erro ao enviar comando de download.\n");
        return false;
    }
    
    // Get server response
    packet response;
    if (read(server_socket, &response, sizeof(packet)) <= 0) {
        printf("Erro ao receber resposta do servidor.\n");
        return false;
    }
    
    if (strcmp(response.payload, "OK") != 0) {
        printf("Erro ao baixar arquivo: %s\n", response.payload);
        return false;
    }
    
    // Allocate buffer for file
    size_t fileSize = response.total_size;
    char* fileData = new char[fileSize];
    size_t bytesRead = 0;
    
    // Receive file data
    while (bytesRead < fileSize) {
        packet dataPkt;
        if (read(server_socket, &dataPkt, sizeof(packet)) <= 0) {
            printf("Erro ao receber dados do arquivo.\n");
            delete[] fileData;
            return false;
        }
        
        if (dataPkt.type != DATA_PACKET) {
            printf("Pacote inesperado durante o download.\n");
            delete[] fileData;
            return false;
        }
        
        memcpy(fileData + bytesRead, dataPkt.payload, dataPkt.length);
        bytesRead += dataPkt.length;
    }
    
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
    
    // Update mtime in our tracking
    struct stat st;
    if (stat(destPath.c_str(), &st) == 0) {
        file_mutex.lock();
        file_mtimes[filename] = st.st_mtime;
        file_mutex.unlock();
    }
    
    printf("Arquivo '%s' baixado com sucesso.\n", filename.c_str());
    return true;
}

bool delete_file(const std::string& filename) {
    // Send delete command
    packet cmd;
    cmd.type = CMD_DELETE;
    cmd.seqn = 0;
    cmd.total_size = 0;
    strcpy(cmd.payload, filename.c_str());
    cmd.length = filename.length();
    
    if (write(server_socket, &cmd, sizeof(packet)) <= 0) {
        printf("Erro ao enviar comando de exclusão.\n");
        return false;
    }
    
    // Get server response
    packet response;
    if (read(server_socket, &response, sizeof(packet)) <= 0) {
        printf("Erro ao receber resposta do servidor.\n");
        return false;
    }
    
    if (strcmp(response.payload, "OK") == 0) {
        printf("Arquivo '%s' excluído com sucesso.\n", filename.c_str());
        return true;
    } else {
        printf("Erro ao excluir arquivo: %s\n", response.payload);
        return false;
    }
}

void list_server_files() {
    // Send list command
    packet cmd;
    cmd.type = CMD_LIST_SERVER;
    cmd.seqn = 0;
    cmd.total_size = 0;
    cmd.length = 0;
    
    if (write(server_socket, &cmd, sizeof(packet)) <= 0) {
        printf("Erro ao enviar comando de listagem.\n");
        return;
    }
    
    // Get server response
    packet response;
    if (read(server_socket, &response, sizeof(packet)) <= 0) {
        printf("Erro ao receber resposta do servidor.\n");
        return;
    }
    
    // Collect file list
    std::string fileList(response.payload);
    size_t expectedSize = response.total_size;
    
    // If the list is bigger than the initial packet, receive additional data
    while (fileList.size() < expectedSize) {
        packet dataPkt;
        if (read(server_socket, &dataPkt, sizeof(packet)) <= 0) {
            break;
        }
        
        if (dataPkt.type != DATA_PACKET) {
            break;
        }
        
        fileList.append(dataPkt.payload, dataPkt.length);
    }
    
    // Display file list
    printf("Arquivos no servidor:\n");
    printf("Nome\t\tTamanho\t\tModificado em\t\tCriado em\n");
    
    std::string line;
    std::istringstream stream(fileList);
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        
        // Parse CSV format: filename,size,mtime,atime,ctime
        std::string filename;
        size_t size;
        time_t mtime, atime, ctime;
        
        size_t pos = 0;
        std::string token;
        
        // Get filename
        pos = line.find(',');
        filename = line.substr(0, pos);
        line.erase(0, pos + 1);
        
        // Get size
        pos = line.find(',');
        size = std::stoul(line.substr(0, pos));
        line.erase(0, pos + 1);
        
        // Get mtime
        pos = line.find(',');
        mtime = std::stol(line.substr(0, pos));
        line.erase(0, pos + 1);
        
        // Get atime
        pos = line.find(',');
        atime = std::stol(line.substr(0, pos));
        line.erase(0, pos + 1);
        
        // Get ctime
        ctime = std::stol(line);
        
        // Display file info
        char mtime_str[30], ctime_str[30];
        strftime(mtime_str, sizeof(mtime_str), "%Y-%m-%d %H:%M:%S", localtime(&mtime));
        strftime(ctime_str, sizeof(ctime_str), "%Y-%m-%d %H:%M:%S", localtime(&ctime));
        
        printf("%s\t\t%zu bytes\t%s\t%s\n", 
               filename.c_str(), size, mtime_str, ctime_str);
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
    // Make sure sync dir exists
    watch_sync_dir();
    
    // Send get_sync_dir command
    packet cmd;
    cmd.type = CMD_GET_SYNC_DIR;
    cmd.seqn = 0;
    cmd.total_size = 0;
    cmd.length = 0;
    
    if (write(server_socket, &cmd, sizeof(packet)) <= 0) {
        printf("Erro ao enviar comando get_sync_dir.\n");
        return;
    }
    
    // Get server response
    packet response;
    if (read(server_socket, &response, sizeof(packet)) <= 0) {
        printf("Erro ao receber resposta do servidor.\n");
        return;
    }
    
    if (strcmp(response.payload, "OK") != 0) {
        printf("Erro ao inicializar diretório de sincronização: %s\n", response.payload);
        return;
    }
    
    // Get number of files
    size_t numFiles = response.total_size;
    printf("Sincronizando %zu arquivos do servidor...\n", numFiles);
    
    // Download each file
    for (size_t i = 0; i < numFiles; i++) {
        packet filePkt;
        if (read(server_socket, &filePkt, sizeof(packet)) <= 0) {
            printf("Erro ao receber informações de arquivo do servidor.\n");
            return;
        }
        
        if (filePkt.type != SYNC_NOTIFICATION) {
            continue;
        }
        
        std::string filename(filePkt.payload, filePkt.length);
        download_file(filename);
    }
    
    printf("Diretório de sincronização inicializado com sucesso.\n");
}
