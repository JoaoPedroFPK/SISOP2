#ifndef REQUEST_ROUTER_H
#define REQUEST_ROUTER_H

#include "sync_protocol.h"
#include "replication_protocol.h"
#include <vector>
#include <map>
#include <mutex>

enum class RequestType {
    WRITE_OPERATION,  // Must go to primary (LOGIN, UPLOAD, DELETE)
    READ_OPERATION,   // Can go to any server (DOWNLOAD, LIST)
    METADATA_OPERATION // Usually primary (GET_SYNC_DIR)
};

class RequestRouter {
public:
    RequestRouter();
    
    // Request routing logic
    RequestType classifyRequest(const Message& request) const;
    int selectTargetServer(RequestType type, const std::vector<ServerInfo>& availableServers, 
                          int primaryServerId) const;
    
    // Load balancing for read operations
    int selectReadServer(const std::vector<ServerInfo>& availableServers) const;
    
    // Connection pool management
    int getPooledConnection(int serverId);
    void returnConnection(int serverId, int sockfd);
    void closeAllConnections();
    void cleanupConnectionPool(int serverId);
    
    // Statistics and monitoring
    void recordRequest(int serverId, RequestType type);
    void recordLatency(int serverId, std::chrono::milliseconds latency);
    std::map<int, int> getServerLoadStats();
    
private:
    // Load balancing state
    mutable int roundRobinIndex;
    mutable std::mutex routingMutex;
    
    // Connection pooling
    std::map<int, std::vector<int>> connectionPools;  // serverId -> vector of socket fds
    std::mutex poolMutex;
    static const size_t MAX_POOL_SIZE = 5;
    
    // Statistics
    std::map<int, int> serverRequestCount;
    std::map<int, std::chrono::milliseconds> serverTotalLatency;
    std::mutex statsMutex;
    
    // Helper methods
    bool isWriteOperation(Command cmd) const;
    bool isReadOperation(Command cmd) const;
    int createNewConnection(const ServerInfo& serverInfo);
};

#endif // REQUEST_ROUTER_H 