#ifndef SYNC_SERVER_H
#define SYNC_SERVER_H

#include <string>
#include <thread>
#include <atomic>
#include "sync_protocol.h"
#include "file_manager.h"
#include "client_manager.h"

class SyncServer {
public:
    SyncServer();
    ~SyncServer();
    
    void run(int port);
    void stop();
    
private:
    SyncProtocol protocol;
    FileManager fileManager;
    ClientManager clientManager;
    std::atomic<bool> running;
    int serverSocket;
    
    void handleClient(int clientSocket);
    void processCommand(int sockfd, const std::string& username, const Message& msg);
    
    // Command handlers
    Result handleUpload(int sockfd, const std::string& username, const Message& msg);
    Result handleDownload(int sockfd, const std::string& username, const Message& msg);
    Result handleDelete(int sockfd, const std::string& username, const Message& msg);
    Result handleListServer(int sockfd, const std::string& username);
    Result handleGetSyncDir(int sockfd, const std::string& username);
    
    // Helper methods
    Result sendFileData(int sockfd, const std::vector<uint8_t>& data);
    Result receiveFileData(int sockfd, size_t expectedSize, std::vector<uint8_t>& data);
    std::string formatFileList(const std::vector<FileInfo>& files);
};

// Global function to start server (for compatibility)
void run_server(int port);

#endif // SYNC_SERVER_H 