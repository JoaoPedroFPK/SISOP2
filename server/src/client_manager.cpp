#include "client_manager.h"
#include "common.h"
#include <algorithm>
#include <iostream>

ClientManager::ClientManager() {}

ClientManager::~ClientManager() {}

Result ClientManager::addClient(const std::string& username, int sockfd) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    
    // Check session limit
    auto it = userClients.find(username);
    if (it != userClients.end() && it->second.size() >= MAX_SESSIONS_PER_USER) {
        printf("Usuário %s atingiu o limite de %d sessões\n", username.c_str(), MAX_SESSIONS_PER_USER);
        return Result(SyncError::SESSION_LIMIT_REACHED, "Session limit reached");
    }
    
    // Add client
    ClientInfo info;
    info.username = username;
    info.sockfd = sockfd;
    info.active = true;
    
    userClients[username].push_back(info);
    socketToUser[sockfd] = username;
    
    printf("Cliente %s conectado (socket %d, sessões ativas: %zu)\n", 
           username.c_str(), sockfd, userClients[username].size());
    
    return Result(SyncError::SUCCESS);
}

void ClientManager::removeClient(const std::string& username, int sockfd) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    
    auto it = userClients.find(username);
    if (it != userClients.end()) {
        auto& clients = it->second;
        clients.erase(
            std::remove_if(clients.begin(), clients.end(),
                [sockfd](const ClientInfo& client) {
                    return client.sockfd == sockfd;
                }),
            clients.end());
        
        if (clients.empty()) {
            userClients.erase(it);
        }
    }
    
    socketToUser.erase(sockfd);
    printf("Cliente %s desconectado (socket %d)\n", username.c_str(), sockfd);
}

void ClientManager::removeClient(int sockfd) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    
    auto socketIt = socketToUser.find(sockfd);
    if (socketIt != socketToUser.end()) {
        std::string username = socketIt->second;
        removeClient(username, sockfd);
    }
}

void ClientManager::broadcastToUser(const std::string& username, const Message& msg, int excludeFd) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    
    auto it = userClients.find(username);
    if (it == userClients.end()) {
        return;
    }
    
    for (const auto& client : it->second) {
        if (client.active && client.sockfd != excludeFd) {
            Result result = protocol.sendMessage(client.sockfd, msg);
            if (!result.success()) {
                printf("Falha ao enviar mensagem para cliente %s (socket %d): %s\n",
                       username.c_str(), client.sockfd, result.getMessage().c_str());
            }
        }
    }
}

void ClientManager::notifyUserFileChange(const std::string& username, const std::string& filename, 
                                        char action, int excludeFd) {
    Message notification;
    notification.command = Command::NOTIFICATION;
    notification.payload = std::string(1, action) + ":" + filename;
    notification.sequence = 0;
    notification.total_size = 0;
    
    broadcastToUser(username, notification, excludeFd);
}

std::vector<int> ClientManager::getUserSockets(const std::string& username) const {
    std::lock_guard<std::mutex> lock(clientsMutex);
    std::vector<int> sockets;
    
    auto it = userClients.find(username);
    if (it != userClients.end()) {
        for (const auto& client : it->second) {
            if (client.active) {
                sockets.push_back(client.sockfd);
            }
        }
    }
    
    return sockets;
}

int ClientManager::getUserSessionCount(const std::string& username) const {
    std::lock_guard<std::mutex> lock(clientsMutex);
    
    auto it = userClients.find(username);
    if (it != userClients.end()) {
        return it->second.size();
    }
    
    return 0;
}

bool ClientManager::isUserConnected(const std::string& username) const {
    std::lock_guard<std::mutex> lock(clientsMutex);
    
    auto it = userClients.find(username);
    return (it != userClients.end() && !it->second.empty());
} 