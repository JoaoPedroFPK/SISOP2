#ifndef CLUSTER_MANAGER_H
#define CLUSTER_MANAGER_H

#include "replication_protocol.h"
#include "cluster_config.h"
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

// Forward declaration to avoid circular dependency
class ElectionManager;

class ClusterManager {
public:
    ClusterManager();
    ~ClusterManager();
    
    // Initialization
    bool initialize(const ClusterConfig& config, int thisServerId);
    void start();
    void stop();
    
    // Cluster topology management
    std::vector<ServerInfo> getActiveServers() const;
    ServerInfo getPrimaryServer() const;
    std::vector<ServerInfo> getBackupServers() const;
    bool isPrimary() const;
    int getCurrentPrimaryId() const;
    int getThisServerId() const { return thisServerId; }
    
    // Server status management
    bool updateServerStatus(int serverId, bool isActive);
    bool promoteServerToPrimary(int serverId);
    bool isServerAlive(int serverId) const;
    
    // Connection management
    int connectToServer(int serverId);
    void disconnectFromServer(int serverId);
    std::map<int, int> getServerConnections() const;
    
    // Health monitoring
    void sendHeartbeatToServers();
    void processHeartbeatResponse(int serverId, const ServerInfo& serverInfo);
    
    // Front-end communication
    void registerWithFrontEnd(const std::string& frontEndAddress, int frontEndPort);
    void notifyPrimaryChange(int newPrimaryId);
    
private:
    // Configuration and state
    ClusterConfig clusterConfig;
    std::vector<ServerInfo> serverCluster;
    int thisServerId;
    int currentPrimaryId;
    bool isRunning;
    
    // Election management
    ElectionManager* electionManager;
    
    // Communication
    ReplicationProtocol replicationProtocol;
    std::map<int, int> serverConnections;  // serverId -> socket fd
    
    // Threading and synchronization
    std::thread heartbeatThread;
    std::thread monitoringThread;
    mutable std::mutex clusterMutex;
    std::atomic<bool> shouldStop;
    
    // Timing
    std::chrono::milliseconds heartbeatInterval;
    std::chrono::milliseconds failureTimeout;
    
    // Helper methods
    void heartbeatWorker();
    void monitoringWorker();
    void handleServerFailure(int serverId);
    void updateServerHeartbeat(int serverId);
    bool establishServerConnection(int serverId);
    void closeServerConnection(int serverId);
    
    // Cluster state management
    void updateClusterTopology();
    ServerInfo* findServerById(int serverId);
    void electNewPrimary();
};

#endif // CLUSTER_MANAGER_H 