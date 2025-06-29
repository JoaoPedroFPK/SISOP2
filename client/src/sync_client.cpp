#include "sync_client.h"
#include "socket_utils.h"
#include "common.h"
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <chrono>

namespace fs = std::filesystem;

SyncClient::SyncClient() : sockfd(-1), connected(false), shouldStop(false) {}

SyncClient::~SyncClient() {
    disconnect();
}

Result SyncClient::connect(const std::string& user, const std::string& ip, int port) {
    if (connected.load()) {
        return Result(SyncError::PROTOCOL_ERROR, "Already connected");
    }
    
    username = user;
    syncDirPath = "sync_dir_" + username;
    
    // Create socket
    sockfd = create_socket();
    if (sockfd < 0) {
        return Result(SyncError::CONNECTION_LOST, "Failed to create socket");
    }
    
    // Connect to server
    if (connect_socket(sockfd, ip.c_str(), port) < 0) {
        close(sockfd);
        sockfd = -1;
        return Result(SyncError::CONNECTION_LOST, "Failed to connect to server");
    }
    
    // Send login command
    Result loginResult = protocol.sendCommand(sockfd, Command::LOGIN, username);
    if (!loginResult.success()) {
        close(sockfd);
        sockfd = -1;
        return loginResult;
    }
    
    // Receive login response
    Message response;
    Result responseResult = protocol.receiveResponse(sockfd, response);
    if (!responseResult.success()) {
        close(sockfd);
        sockfd = -1;
        return responseResult;
    }
    
    if (response.command != Command::LOGIN) {
        close(sockfd);
        sockfd = -1;
        return Result(SyncError::PROTOCOL_ERROR, "Invalid login response");
    }
    
    // Setup sync directory
    if (!setupSyncDirectory()) {
        close(sockfd);
        sockfd = -1;
        return Result(SyncError::PERMISSION_DENIED, "Failed to setup sync directory");
    }
    
    connected.store(true);
    shouldStop.store(false);
    
    // Start background threads
    notificationThread = std::thread(&SyncClient::handleNotifications, this);
    fileMonitorThread = std::thread(&SyncClient::monitorFileChanges, this);
    
    // Get initial sync
    getSyncDir();
    
    printf("Conectado ao servidor %s:%d como %s\n", ip.c_str(), port, username.c_str());
    
    return Result(SyncError::SUCCESS);
}

void SyncClient::disconnect() {
    if (!connected.load()) {
        return;
    }
    
    connected.store(false);
    shouldStop.store(true);
    
    // Send exit command
    if (sockfd >= 0) {
        protocol.sendCommand(sockfd, Command::EXIT);
        close(sockfd);
        sockfd = -1;
    }
    
    // Wait for threads to finish
    notificationCv.notify_all();
    if (notificationThread.joinable()) {
        notificationThread.join();
    }
    if (fileMonitorThread.joinable()) {
        fileMonitorThread.join();
    }
    
    printf("Desconectado do servidor\n");
}

bool SyncClient::isConnected() const {
    return connected.load();
}

Result SyncClient::uploadFile(const std::string& filepath) {
    if (!connected.load()) {
        return Result(SyncError::CONNECTION_LOST, "Not connected");
    }
    
    if (!fs::exists(filepath)) {
        return Result(SyncError::FILE_NOT_FOUND, "File not found: " + filepath);
    }
    
    std::string filename = getFilename(filepath);
    
    // Read file data
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        return Result(SyncError::PERMISSION_DENIED, "Cannot open file: " + filepath);
    }
    
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> fileData(fileSize);
    file.read(reinterpret_cast<char*>(fileData.data()), fileSize);
    file.close();
    
    // Send upload command
    Result cmdResult = protocol.sendCommand(sockfd, Command::UPLOAD, filename, fileData);
    if (!cmdResult.success()) {
        return cmdResult;
    }
    
    // Send file data in chunks
    Result dataResult = sendFileData(fileData);
    if (!dataResult.success()) {
        return dataResult;
    }
    
    // Receive response
    Message response;
    Result respResult = protocol.receiveResponse(sockfd, response);
    if (!respResult.success()) {
        return respResult;
    }
    
    if (response.payload == "OK") {
        printf("Arquivo '%s' enviado com sucesso\n", filename.c_str());
        return Result(SyncError::SUCCESS);
    } else {
        return Result(SyncError::PROTOCOL_ERROR, "Upload failed: " + response.payload);
    }
}

Result SyncClient::downloadFile(const std::string& filename) {
    if (!connected.load()) {
        return Result(SyncError::CONNECTION_LOST, "Not connected");
    }
    
    // Send download command
    Result cmdResult = protocol.sendCommand(sockfd, Command::DOWNLOAD, filename);
    if (!cmdResult.success()) {
        return cmdResult;
    }
    
    // Receive response
    Message response;
    Result respResult = protocol.receiveResponse(sockfd, response);
    if (!respResult.success()) {
        return respResult;
    }
    
    if (response.payload != "OK") {
        return Result(SyncError::FILE_NOT_FOUND, "Download failed: " + response.payload);
    }
    
    // Receive file data
    std::vector<uint8_t> fileData;
    Result dataResult = receiveFileData(response.total_size, fileData);
    if (!dataResult.success()) {
        return dataResult;
    }
    
    // Save file
    std::string filepath = filename; // Save to current directory
    std::ofstream file(filepath, std::ios::binary);
    if (!file) {
        return Result(SyncError::PERMISSION_DENIED, "Cannot create file: " + filepath);
    }
    
    file.write(reinterpret_cast<const char*>(fileData.data()), fileData.size());
    file.close();
    
    printf("Arquivo '%s' baixado com sucesso\n", filename.c_str());
    return Result(SyncError::SUCCESS);
}

Result SyncClient::deleteFile(const std::string& filename) {
    if (!connected.load()) {
        return Result(SyncError::CONNECTION_LOST, "Not connected");
    }
    
    // Send delete command
    Result cmdResult = protocol.sendCommand(sockfd, Command::DELETE, filename);
    if (!cmdResult.success()) {
        return cmdResult;
    }
    
    // Receive response
    Message response;
    Result respResult = protocol.receiveResponse(sockfd, response);
    if (!respResult.success()) {
        return respResult;
    }
    
    if (response.payload == "OK") {
        printf("Arquivo '%s' deletado com sucesso\n", filename.c_str());
        
        // Remove from local sync directory if exists
        std::string localPath = syncDirPath + "/" + filename;
        if (fs::exists(localPath)) {
            fs::remove(localPath);
            
            std::lock_guard<std::mutex> lock(fileTimeMutex);
            fileModTimes.erase(filename);
        }
        
        return Result(SyncError::SUCCESS);
    } else {
        return Result(SyncError::FILE_NOT_FOUND, "Delete failed: " + response.payload);
    }
}

Result SyncClient::listServerFiles() {
    if (!connected.load()) {
        return Result(SyncError::CONNECTION_LOST, "Not connected");
    }
    
    // Send list server command
    Result cmdResult = protocol.sendCommand(sockfd, Command::LIST_SERVER);
    if (!cmdResult.success()) {
        return cmdResult;
    }
    
    // Receive response
    Message response;
    Result respResult = protocol.receiveResponse(sockfd, response);
    if (!respResult.success()) {
        return respResult;
    }
    
    // Receive additional data if needed
    std::string fileList = response.payload;
    if (response.total_size > 0 && fileList.length() < response.total_size) {
        std::vector<uint8_t> additionalData;
        Result dataResult = receiveFileData(response.total_size - fileList.length(), additionalData);
        if (dataResult.success()) {
            fileList.append(reinterpret_cast<const char*>(additionalData.data()), additionalData.size());
        }
    }
    
    // Print file list
    if (fileList.empty()) {
        printf("Nenhum arquivo no servidor\n");
        return Result(SyncError::SUCCESS);
    }
    
    printf("Arquivos no servidor:\n");
    printf("%-30s %-10s %-20s\n", "Nome", "Tamanho", "Modificado");
    
    std::istringstream stream(fileList);
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream lineStream(line);
        std::string filename, sizeStr, mtimeStr;
        
        std::getline(lineStream, filename, ',');
        std::getline(lineStream, sizeStr, ',');
        std::getline(lineStream, mtimeStr, ',');
        
        if (!filename.empty() && !sizeStr.empty() && !mtimeStr.empty()) {
            time_t mtime = std::stol(mtimeStr);
            char timeStr[64];
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&mtime));
            
            printf("%-30s %-10s %-20s\n", filename.c_str(), sizeStr.c_str(), timeStr);
        }
    }
    
    return Result(SyncError::SUCCESS);
}

Result SyncClient::listClientFiles() {
    if (!fs::exists(syncDirPath)) {
        printf("Diretório de sincronização não existe\n");
        return Result(SyncError::SUCCESS);
    }
    
    printf("Arquivos locais em %s:\n", syncDirPath.c_str());
    printf("%-30s %-10s %-20s\n", "Nome", "Tamanho", "Modificado");
    
    for (const auto& entry : fs::directory_iterator(syncDirPath)) {
        if (entry.is_regular_file()) {
            struct stat st;
            std::string filepath = entry.path().string();
            
            if (stat(filepath.c_str(), &st) == 0) {
                char timeStr[64];
                strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&st.st_mtime));
                
                printf("%-30s %-10lld %-20s\n",
                       entry.path().filename().c_str(),
                       (long long)st.st_size,
                       timeStr);
            }
        }
    }
    
    return Result(SyncError::SUCCESS);
}

Result SyncClient::getSyncDir() {
    if (!connected.load()) {
        return Result(SyncError::CONNECTION_LOST, "Not connected");
    }
    
    // Send get sync dir command
    Result cmdResult = protocol.sendCommand(sockfd, Command::GET_SYNC_DIR);
    if (!cmdResult.success()) {
        return cmdResult;
    }
    
    // Receive response
    Message response;
    Result respResult = protocol.receiveResponse(sockfd, response);
    if (!respResult.success()) {
        return respResult;
    }
    
    if (response.payload != "OK") {
        return Result(SyncError::PROTOCOL_ERROR, "Get sync dir failed: " + response.payload);
    }
    
    printf("Sincronizando %u arquivos...\n", response.total_size);
    
    // Receive file notifications
    for (uint32_t i = 0; i < response.total_size; i++) {
        Message notification;
        Result notifResult = protocol.receiveMessage(sockfd, notification);
        if (notifResult.success()) {
            processNotification(notification);
        }
    }
    
    updateFileModTimes();
    printf("Sincronização inicial concluída\n");
    
    return Result(SyncError::SUCCESS);
}

std::string SyncClient::getSyncDirPath() const {
    return syncDirPath;
}

void SyncClient::handleNotifications() {
    while (!shouldStop.load()) {
        Message notification;
        Result result = protocol.receiveMessage(sockfd, notification, 1000); // 1 second timeout
        
        if (result.success()) {
            if (notification.command == Command::NOTIFICATION) {
                processNotification(notification);
            }
        } else if (result.getError() == SyncError::CONNECTION_LOST) {
            connected.store(false);
            break;
        }
        // Timeout is expected, continue loop
    }
}

void SyncClient::monitorFileChanges() {
    while (!shouldStop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        if (!connected.load() || !fs::exists(syncDirPath)) {
            continue;
        }
        
        std::unordered_map<std::string, time_t> currentTimes;
        
        // Check current files
        for (const auto& entry : fs::directory_iterator(syncDirPath)) {
            if (entry.is_regular_file()) {
                struct stat st;
                std::string filepath = entry.path().string();
                std::string filename = entry.path().filename().string();
                
                if (stat(filepath.c_str(), &st) == 0) {
                    currentTimes[filename] = st.st_mtime;
                    
                    std::lock_guard<std::mutex> lock(fileTimeMutex);
                    auto it = fileModTimes.find(filename);
                    if (it == fileModTimes.end() || it->second != st.st_mtime) {
                        // File is new or modified
                        uploadFile(filepath);
                    }
                }
            }
        }
        
        // Update stored times
        std::lock_guard<std::mutex> lock(fileTimeMutex);
        fileModTimes = currentTimes;
    }
}

void SyncClient::processNotification(const Message& msg) {
    std::string payload = msg.payload;
    size_t colonPos = payload.find(':');
    
    if (colonPos == std::string::npos) {
        return;
    }
    
    char action = payload[0];
    std::string filename = payload.substr(colonPos + 1);
    std::string filepath = syncDirPath + "/" + filename;
    
    if (action == 'U') {
        // Download updated file
        printf("Arquivo %s atualizado no servidor, baixando...\n", filename.c_str());
        
        Result cmdResult = protocol.sendCommand(sockfd, Command::DOWNLOAD, filename);
        if (!cmdResult.success()) return;
        
        Message response;
        Result respResult = protocol.receiveResponse(sockfd, response);
        if (!respResult.success()) return;
        
        if (response.payload == "OK") {
            std::vector<uint8_t> fileData;
            Result dataResult = receiveFileData(response.total_size, fileData);
            if (dataResult.success()) {
                std::ofstream file(filepath, std::ios::binary);
                if (file) {
                    file.write(reinterpret_cast<const char*>(fileData.data()), fileData.size());
                    file.close();
                    
                    // Update modification time
                    struct stat st;
                    if (stat(filepath.c_str(), &st) == 0) {
                        std::lock_guard<std::mutex> lock(fileTimeMutex);
                        fileModTimes[filename] = st.st_mtime;
                    }
                    
                    printf("Arquivo %s baixado com sucesso\n", filename.c_str());
                }
            }
        }
    } else if (action == 'D') {
        // Delete file locally
        printf("Arquivo %s removido no servidor, removendo localmente...\n", filename.c_str());
        if (fs::exists(filepath)) {
            fs::remove(filepath);
            
            std::lock_guard<std::mutex> lock(fileTimeMutex);
            fileModTimes.erase(filename);
            
            printf("Arquivo %s removido localmente\n", filename.c_str());
        }
    }
}

void SyncClient::updateFileModTimes() {
    std::lock_guard<std::mutex> lock(fileTimeMutex);
    fileModTimes.clear();
    
    if (!fs::exists(syncDirPath)) {
        return;
    }
    
    for (const auto& entry : fs::directory_iterator(syncDirPath)) {
        if (entry.is_regular_file()) {
            struct stat st;
            std::string filepath = entry.path().string();
            std::string filename = entry.path().filename().string();
            
            if (stat(filepath.c_str(), &st) == 0) {
                fileModTimes[filename] = st.st_mtime;
            }
        }
    }
}

bool SyncClient::setupSyncDirectory() {
    if (!fs::exists(syncDirPath)) {
        return fs::create_directory(syncDirPath);
    }
    return true;
}

Result SyncClient::sendFileData(const std::vector<uint8_t>& data) {
    size_t offset = 0;
    size_t chunkSize = sizeof(((packet*)0)->payload);
    
    while (offset < data.size()) {
        size_t currentChunk = std::min(chunkSize, data.size() - offset);
        std::vector<uint8_t> chunk(data.begin() + offset, data.begin() + offset + currentChunk);
        
        Message dataMsg;
        dataMsg.command = Command::DATA_PACKET;
        dataMsg.data = chunk;
        dataMsg.sequence = offset / chunkSize + 1;
        dataMsg.total_size = currentChunk;
        
        Result result = protocol.sendMessage(sockfd, dataMsg);
        if (!result.success()) {
            return result;
        }
        
        offset += currentChunk;
    }
    
    return Result(SyncError::SUCCESS);
}

Result SyncClient::receiveFileData(size_t expectedSize, std::vector<uint8_t>& data) {
    data.clear();
    data.reserve(expectedSize);
    
    while (data.size() < expectedSize) {
        Message dataMsg;
        Result result = protocol.receiveMessage(sockfd, dataMsg);
        if (!result.success()) {
            return result;
        }
        
        if (dataMsg.command != Command::DATA_PACKET) {
            return Result(SyncError::PROTOCOL_ERROR, "Expected data packet");
        }
        
        data.insert(data.end(), dataMsg.data.begin(), dataMsg.data.end());
    }
    
    return Result(SyncError::SUCCESS);
}

std::string SyncClient::getFilename(const std::string& filepath) {
    return fs::path(filepath).filename().string();
} 