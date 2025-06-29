#include "request_router.h"
#include "socket_utils.h"
#include <algorithm>
#include <random>
#include <unistd.h>

RequestRouter::RequestRouter() : roundRobinIndex(0) {}

RequestType RequestRouter::classifyRequest(const Message& request) const {
    if (isWriteOperation(request.command)) {
        return RequestType::WRITE_OPERATION;
    } else if (isReadOperation(request.command)) {
        return RequestType::READ_OPERATION;
    } else {
        return RequestType::METADATA_OPERATION;
    }
}

int RequestRouter::selectTargetServer(RequestType type, const std::vector<ServerInfo>& availableServers, 
                                     int primaryServerId) const {
    if (availableServers.empty()) {
        return -1;
    }
    
    switch (type) {
        case RequestType::WRITE_OPERATION:
        case RequestType::METADATA_OPERATION:
            // Must use primary server
            return primaryServerId;
            
        case RequestType::READ_OPERATION:
            // Can use any server, apply load balancing
            return selectReadServer(availableServers);
            
        default:
            return primaryServerId;
    }
}

int RequestRouter::selectReadServer(const std::vector<ServerInfo>& availableServers) const {
    if (availableServers.empty()) {
        return -1;
    }
    
    std::lock_guard<std::mutex> lock(routingMutex);
    
    // Round-robin load balancing
    int selectedIndex = roundRobinIndex % availableServers.size();
    roundRobinIndex = (roundRobinIndex + 1) % availableServers.size();
    
    return availableServers[selectedIndex].serverId;
}

int RequestRouter::getPooledConnection(int serverId) {
    std::lock_guard<std::mutex> lock(poolMutex);
    
    auto it = connectionPools.find(serverId);
    if (it != connectionPools.end() && !it->second.empty()) {
        int sockfd = it->second.back();
        it->second.pop_back();
        return sockfd;
    }
    
    return -1; // No pooled connection available
}

void RequestRouter::returnConnection(int serverId, int sockfd) {
    std::lock_guard<std::mutex> lock(poolMutex);
    
    auto& pool = connectionPools[serverId];
    if (pool.size() < MAX_POOL_SIZE) {
        pool.push_back(sockfd);
    } else {
        // Pool is full, close the connection
        close(sockfd);
    }
}

void RequestRouter::closeAllConnections() {
    std::lock_guard<std::mutex> lock(poolMutex);
    
    for (auto& pair : connectionPools) {
        for (int sockfd : pair.second) {
            close(sockfd);
        }
        pair.second.clear();
    }
    connectionPools.clear();
}

void RequestRouter::recordRequest(int serverId, RequestType /*type*/) {
    std::lock_guard<std::mutex> lock(statsMutex);
    serverRequestCount[serverId]++;
}

void RequestRouter::recordLatency(int serverId, std::chrono::milliseconds latency) {
    std::lock_guard<std::mutex> lock(statsMutex);
    serverTotalLatency[serverId] += latency;
}

std::map<int, int> RequestRouter::getServerLoadStats() {
    std::lock_guard<std::mutex> lock(statsMutex);
    return serverRequestCount;
}

bool RequestRouter::isWriteOperation(Command cmd) const {
    return cmd == Command::LOGIN || 
           cmd == Command::UPLOAD || 
           cmd == Command::DELETE;
}

bool RequestRouter::isReadOperation(Command cmd) const {
    return cmd == Command::DOWNLOAD || 
           cmd == Command::LIST_SERVER || 
           cmd == Command::LIST_CLIENT;
}

int RequestRouter::createNewConnection(const ServerInfo& serverInfo) {
    return create_client_socket(serverInfo.address, serverInfo.port);
}

void RequestRouter::cleanupConnectionPool(int serverId) {
    std::lock_guard<std::mutex> lock(poolMutex);
    
    auto it = connectionPools.find(serverId);
    if (it != connectionPools.end()) {
        for (int sockfd : it->second) {
            close(sockfd);
        }
        connectionPools.erase(it);
    }
} 