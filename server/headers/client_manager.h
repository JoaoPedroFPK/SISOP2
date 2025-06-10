#ifndef CLIENT_MANAGER_H
#define CLIENT_MANAGER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "sync_protocol.h"

struct ClientInfo {
    std::string username;
    int sockfd;
    bool active;
};

class ClientManager {
public:
    ClientManager();
    ~ClientManager();
    
    // Client management
    Result addClient(const std::string& username, int sockfd);
    void removeClient(const std::string& username, int sockfd);
    void removeClient(int sockfd);
    
    // Broadcasting
    void broadcastToUser(const std::string& username, const Message& msg, int excludeFd = -1);
    void notifyUserFileChange(const std::string& username, const std::string& filename, 
                             char action, int excludeFd = -1);
    
    // Query
    std::vector<int> getUserSockets(const std::string& username) const;
    int getUserSessionCount(const std::string& username) const;
    bool isUserConnected(const std::string& username) const;
    
private:
    mutable std::mutex clientsMutex;
    std::unordered_map<std::string, std::vector<ClientInfo>> userClients;
    std::unordered_map<int, std::string> socketToUser;
    
    static const int MAX_SESSIONS_PER_USER = 2;
    
    SyncProtocol protocol;
};

#endif // CLIENT_MANAGER_H 