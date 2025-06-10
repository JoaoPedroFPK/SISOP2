#include "cluster_manager.h"
#include "socket_utils.h"
#include <iostream>
#include <algorithm>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

ClusterManager::ClusterManager() 
    : thisServerId(-1), currentPrimaryId(-1), isRunning(false), shouldStop(false),
      heartbeatInterval(2000), failureTimeout(5000) {}

ClusterManager::~ClusterManager() {
    stop();
}

bool ClusterManager::initialize(const ClusterConfig& config, int serverId) {
    std::lock_guard<std::mutex> lock(clusterMutex);
    
    clusterConfig = config;
    thisServerId = serverId;
    serverCluster = config.servers;
    
    // Set timing parameters from config
    heartbeatInterval = std::chrono::milliseconds(config.heartbeatInterval);
    failureTimeout = std::chrono::milliseconds(config.electionTimeout);
    
    // Find primary server or elect one
    auto primaryIt = std::find_if(serverCluster.begin(), serverCluster.end(),
                                 [](const ServerInfo& server) { return server.isPrimary; });
    
    if (primaryIt != serverCluster.end()) {
        currentPrimaryId = primaryIt->serverId;
    } else {
        // No primary configured, elect one based on priority
        electNewPrimary();
    }
    
    std::cout << "ClusterManager initialized for server " << thisServerId 
              << ", primary server: " << currentPrimaryId << std::endl;
    
    return true;
}

void ClusterManager::start() {
    if (isRunning) return;
    
    isRunning = true;
    shouldStop = false;
    
    // Start monitoring threads
    heartbeatThread = std::thread(&ClusterManager::heartbeatWorker, this);
    monitoringThread = std::thread(&ClusterManager::monitoringWorker, this);
    
    std::cout << "ClusterManager started" << std::endl;
}

void ClusterManager::stop() {
    if (!isRunning) return;
    
    shouldStop = true;
    isRunning = false;
    
    // Close all server connections
    for (auto& connection : serverConnections) {
        close(connection.second);
    }
    serverConnections.clear();
    
    // Join threads
    if (heartbeatThread.joinable()) {
        heartbeatThread.join();
    }
    if (monitoringThread.joinable()) {
        monitoringThread.join();
    }
    
    std::cout << "ClusterManager stopped" << std::endl;
}

std::vector<ServerInfo> ClusterManager::getActiveServers() const {
    std::lock_guard<std::mutex> lock(clusterMutex);
    std::vector<ServerInfo> activeServers;
    
    std::copy_if(serverCluster.begin(), serverCluster.end(),
                std::back_inserter(activeServers),
                [](const ServerInfo& server) { return server.isActive; });
    
    return activeServers;
}

ServerInfo ClusterManager::getPrimaryServer() const {
    std::lock_guard<std::mutex> lock(clusterMutex);
    
    auto it = std::find_if(serverCluster.begin(), serverCluster.end(),
                          [this](const ServerInfo& server) { 
                              return server.serverId == currentPrimaryId; 
                          });
    
    return (it != serverCluster.end()) ? *it : ServerInfo();
}

std::vector<ServerInfo> ClusterManager::getBackupServers() const {
    std::lock_guard<std::mutex> lock(clusterMutex);
    std::vector<ServerInfo> backupServers;
    
    std::copy_if(serverCluster.begin(), serverCluster.end(),
                std::back_inserter(backupServers),
                [this](const ServerInfo& server) { 
                    return server.isActive && server.serverId != currentPrimaryId; 
                });
    
    return backupServers;
}

bool ClusterManager::isPrimary() const {
    return thisServerId == currentPrimaryId;
}

int ClusterManager::getCurrentPrimaryId() const {
    return currentPrimaryId;
}

bool ClusterManager::updateServerStatus(int serverId, bool isActive) {
    std::lock_guard<std::mutex> lock(clusterMutex);
    
    ServerInfo* server = findServerById(serverId);
    if (!server) return false;
    
    server->isActive = isActive;
    server->lastHeartbeat = std::chrono::steady_clock::now();
    
    if (!isActive && serverId == currentPrimaryId) {
        // Primary failed, need to elect new one
        electNewPrimary();
    }
    
    return true;
}

bool ClusterManager::promoteServerToPrimary(int serverId) {
    std::lock_guard<std::mutex> lock(clusterMutex);
    
    ServerInfo* server = findServerById(serverId);
    if (!server || !server->isActive) return false;
    
    // Demote current primary
    if (currentPrimaryId != -1) {
        ServerInfo* currentPrimary = findServerById(currentPrimaryId);
        if (currentPrimary) {
            currentPrimary->isPrimary = false;
        }
    }
    
    // Promote new primary
    server->isPrimary = true;
    currentPrimaryId = serverId;
    
    std::cout << "Server " << serverId << " promoted to primary" << std::endl;
    return true;
}

bool ClusterManager::isServerAlive(int serverId) const {
    std::lock_guard<std::mutex> lock(clusterMutex);
    
    auto it = std::find_if(serverCluster.begin(), serverCluster.end(),
                          [serverId](const ServerInfo& s) { 
                              return s.serverId == serverId; 
                          });
    
    if (it == serverCluster.end()) return false;
    
    auto now = std::chrono::steady_clock::now();
    auto timeSinceHeartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - it->lastHeartbeat);
    
    return it->isActive && timeSinceHeartbeat < failureTimeout;
}

int ClusterManager::connectToServer(int serverId) {
    const ServerInfo* server = findServerById(serverId);
    if (!server) return -1;
    
    int sockfd = create_client_socket(server->address, server->port);
    if (sockfd >= 0) {
        serverConnections[serverId] = sockfd;
    }
    
    return sockfd;
}

void ClusterManager::disconnectFromServer(int serverId) {
    auto it = serverConnections.find(serverId);
    if (it != serverConnections.end()) {
        close(it->second);
        serverConnections.erase(it);
    }
}

std::map<int, int> ClusterManager::getServerConnections() const {
    return serverConnections;
}

void ClusterManager::sendHeartbeatToServers() {
    std::vector<ServerInfo> servers;
    {
        std::lock_guard<std::mutex> lock(clusterMutex);
        servers = serverCluster;
    }
    
    ServerInfo thisServer = *findServerById(thisServerId);
    
    for (const auto& server : servers) {
        if (server.serverId == thisServerId || !server.isActive) continue;
        
        int sockfd = connectToServer(server.serverId);
        if (sockfd >= 0) {
            Result result = replicationProtocol.sendHeartbeat(sockfd, thisServer);
            if (!result.success()) {
                std::cerr << "Failed to send heartbeat to server " << server.serverId 
                         << ": " << result.getMessage() << std::endl;
                handleServerFailure(server.serverId);
            }
            disconnectFromServer(server.serverId);
        }
    }
}

void ClusterManager::processHeartbeatResponse(int serverId, const ServerInfo& /*serverInfo*/) {
    updateServerHeartbeat(serverId);
    updateServerStatus(serverId, true);
}

void ClusterManager::heartbeatWorker() {
    while (!shouldStop) {
        sendHeartbeatToServers();
        std::this_thread::sleep_for(heartbeatInterval);
    }
}

void ClusterManager::monitoringWorker() {
    while (!shouldStop) {
        std::vector<ServerInfo> servers;
        {
            std::lock_guard<std::mutex> lock(clusterMutex);
            servers = serverCluster;
        }
        
        auto now = std::chrono::steady_clock::now();
        
        for (const auto& server : servers) {
            if (server.serverId == thisServerId) continue;
            
            auto timeSinceHeartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - server.lastHeartbeat);
            
            if (server.isActive && timeSinceHeartbeat > failureTimeout) {
                std::cout << "Server " << server.serverId << " appears to have failed" << std::endl;
                handleServerFailure(server.serverId);
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void ClusterManager::handleServerFailure(int serverId) {
    std::cout << "Handling failure of server " << serverId << std::endl;
    
    updateServerStatus(serverId, false);
    disconnectFromServer(serverId);
    
    if (serverId == currentPrimaryId) {
        std::cout << "Primary server failed, electing new primary" << std::endl;
        electNewPrimary();
    }
}

void ClusterManager::updateServerHeartbeat(int serverId) {
    std::lock_guard<std::mutex> lock(clusterMutex);
    
    ServerInfo* server = findServerById(serverId);
    if (server) {
        server->lastHeartbeat = std::chrono::steady_clock::now();
    }
}

bool ClusterManager::establishServerConnection(int serverId) {
    if (serverConnections.find(serverId) != serverConnections.end()) {
        return true; // Already connected
    }
    
    return connectToServer(serverId) >= 0;
}

void ClusterManager::closeServerConnection(int serverId) {
    disconnectFromServer(serverId);
}

void ClusterManager::updateClusterTopology() {
    // This method would be called when receiving cluster updates
    // For now, it's a placeholder for future dynamic topology changes
}

ServerInfo* ClusterManager::findServerById(int serverId) {
    auto it = std::find_if(serverCluster.begin(), serverCluster.end(),
                          [serverId](const ServerInfo& server) { 
                              return server.serverId == serverId; 
                          });
    
    return (it != serverCluster.end()) ? &(*it) : nullptr;
}

void ClusterManager::electNewPrimary() {
    // Simple election: choose server with highest priority among active servers
    ServerInfo* newPrimary = nullptr;
    int highestPriority = -1;
    
    for (auto& server : serverCluster) {
        if (server.isActive && server.priority > highestPriority) {
            highestPriority = server.priority;
            newPrimary = &server;
        }
    }
    
    if (newPrimary) {
        // Demote current primary
        if (currentPrimaryId != -1) {
            ServerInfo* currentPrimary = findServerById(currentPrimaryId);
            if (currentPrimary) {
                currentPrimary->isPrimary = false;
            }
        }
        
        // Promote new primary
        newPrimary->isPrimary = true;
        currentPrimaryId = newPrimary->serverId;
        
        std::cout << "Elected server " << newPrimary->serverId << " as new primary" << std::endl;
    }
}

void ClusterManager::registerWithFrontEnd(const std::string& frontEndAddress, int frontEndPort) {
    int sockfd = create_client_socket(frontEndAddress, frontEndPort);
    if (sockfd >= 0) {
        ServerInfo thisServer = *findServerById(thisServerId);
        Result result = replicationProtocol.sendServerRegistration(sockfd, thisServer);
        if (result.success()) {
            std::cout << "Registered with front-end server" << std::endl;
        }
        close(sockfd);
    }
}

void ClusterManager::notifyPrimaryChange(int newPrimaryId) {
    // Notify front-end about primary change
    int sockfd = create_client_socket(clusterConfig.frontEndAddress, clusterConfig.frontEndPort);
    if (sockfd >= 0) {
        Result result = replicationProtocol.sendPrimaryUpdate(sockfd, newPrimaryId);
        if (result.success()) {
            std::cout << "Notified front-end about primary change to server " << newPrimaryId << std::endl;
        }
        close(sockfd);
    }
} 