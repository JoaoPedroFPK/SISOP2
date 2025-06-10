#ifndef FRONTEND_SERVER_H
#define FRONTEND_SERVER_H

#include "cluster_config.h"
#include "replication_protocol.h"
#include "request_router.h"
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>

class FrontEndServer {
public:
    FrontEndServer();
    ~FrontEndServer();
    
    // Initialization and lifecycle
    bool initialize(const std::string& configFile);
    void run(int port);
    void stop();
    
    // Server management
    void registerServer(const ServerInfo& serverInfo);
    void unregisterServer(int serverId);
    void updatePrimaryServer(int newPrimaryId);
    
    // Health monitoring
    void startHealthMonitoring();
    void stopHealthMonitoring();
    
private:
    // Configuration and state
    ClusterConfigManager configManager;
    std::map<int, ServerInfo> registeredServers;
    int currentPrimaryId;
    bool isRunning;
    
    // Communication
    ReplicationProtocol replicationProtocol;
    SyncProtocol clientProtocol;
    RequestRouter requestRouter;
    
    // Server socket
    int serverSocket;
    int frontEndPort;
    
    // Threading and synchronization
    std::thread acceptThread;
    std::thread healthMonitorThread;
    std::vector<std::thread> clientThreads;
    mutable std::mutex serverMapMutex;
    std::atomic<bool> shouldStop;
    
    // Connection management
    std::map<int, int> serverConnections;  // serverId -> socket fd
    
    // Main server loop
    void acceptConnections();
    void handleClientConnection(int clientSocket);
    
    // Request handling
    void processClientRequest(int clientSocket, const Message& request);
    Result forwardToPrimary(const Message& msg, Message& response);
    Result forwardToServer(int serverId, const Message& msg, Message& response);
    
    // Server communication
    int connectToPrimaryServer();
    int connectToServer(int serverId);
    void disconnectFromServer(int serverId);
    
    // Health monitoring
    void healthMonitorWorker();
    void checkServerHealth(int serverId);
    void handleServerFailure(int serverId);
    void handlePrimaryFailure();
    
    // Helper methods
    ServerInfo* findServerById(int serverId);
    ServerInfo getPrimaryServer();
    std::vector<ServerInfo> getActiveServers();
    bool isServerHealthy(int serverId);
    
    // Front-end specific message handling
    void handleServerRegistration(int socket);
    void handlePrimaryUpdate(int socket);
    void handleHealthCheck(int socket);
};

#endif // FRONTEND_SERVER_H 