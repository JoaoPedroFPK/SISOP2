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
    virtual ~SyncServer();

    // Public interface for derived classes
    bool initialize(int port);
    virtual void start();
    virtual void run(int port);
    virtual void stop();

    // Public command handlers for replication
    virtual Result handleUpload(const std::string& username, const std::string& filename,
                               const std::vector<uint8_t>& data);
    virtual Result handleDelete(const std::string& username, const std::string& filename);
    virtual Result handleClientCommand(const Message& msg);

    virtual void handleClient(int clientSocket);

protected:
    SyncProtocol protocol;
    FileManager fileManager;
    ClientManager clientManager;
    std::atomic<bool> running;
    int serverSocket;
    int serverPort;

    void processCommand(int sockfd, const std::string& username, const Message& msg);

private:
    // Internal command handlers
    Result handleUploadInternal(int sockfd, const std::string& username, const Message& msg);
    Result handleDownload(int sockfd, const std::string& username, const Message& msg);
    Result handleDeleteInternal(int sockfd, const std::string& username, const Message& msg);
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