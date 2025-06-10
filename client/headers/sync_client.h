#ifndef SYNC_CLIENT_H
#define SYNC_CLIENT_H

#include <string>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include "sync_protocol.h"

class SyncClient {
public:
    SyncClient();
    ~SyncClient();
    
    // Connection management
    Result connect(const std::string& username, const std::string& ip, int port);
    void disconnect();
    bool isConnected() const;
    
    // File operations
    Result uploadFile(const std::string& filepath);
    Result downloadFile(const std::string& filename);
    Result deleteFile(const std::string& filename);
    Result listServerFiles();
    Result listClientFiles();
    Result getSyncDir();
    
    // Sync directory management
    std::string getSyncDirPath() const;
    
private:
    SyncProtocol protocol;
    int sockfd;
    std::string username;
    std::string syncDirPath;
    std::atomic<bool> connected;
    
    // Notification handling
    std::thread notificationThread;
    std::queue<Message> notifications;
    std::mutex notificationMutex;
    std::condition_variable notificationCv;
    std::atomic<bool> shouldStop;
    
    // File monitoring
    std::thread fileMonitorThread;
    std::unordered_map<std::string, time_t> fileModTimes;
    std::mutex fileTimeMutex;
    
    // Private methods
    void handleNotifications();
    void monitorFileChanges();
    void processNotification(const Message& msg);
    void updateFileModTimes();
    bool setupSyncDirectory();
    Result sendFileData(const std::vector<uint8_t>& data);
    Result receiveFileData(size_t expectedSize, std::vector<uint8_t>& data);
    std::string getFilename(const std::string& filepath);
};

#endif // SYNC_CLIENT_H 