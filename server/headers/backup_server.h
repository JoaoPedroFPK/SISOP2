#ifndef BACKUP_SERVER_H
#define BACKUP_SERVER_H

#include "sync_server.h"
#include "operation_log.h"
#include "replication_protocol.h"
#include "cluster_config.h"
#include <thread>
#include <atomic>
#include <queue>

class BackupServer : public SyncServer {
public:
    BackupServer();
    virtual ~BackupServer();
    
    // Initialization
    bool initializeAsBackup(const ClusterConfig& config, int serverId);
    void start() override;
    void stop() override;
    
    // Primary server connection
    void connectToPrimary(const ServerInfo& primary);
    void disconnectFromPrimary();
    bool isPrimaryConnected() const;
    
    // Replication handling
    void applyReplication(const ReplicationMessage& msg);
    void synchronizeState();
    Result processReplicationOperation(const Operation& op);
    
    // Override client command handling to reject direct operations
    Result handleClientCommand(const Message& msg) override;
    
    // Backup-specific operations
    void sendConfirmationToPrimary(uint64_t operationId);
    void requestStateSynchronization();
    
protected:
    // State management
    void processReplicationQueue();
    void confirmOperation(uint64_t operationId);
    void applyOperationToFileSystem(const Operation& op);
    
private:
    // Primary server connection
    ServerInfo primaryServer;
    int primarySocket;
    bool connectedToPrimary;
    std::chrono::time_point<std::chrono::steady_clock> lastPrimaryHeartbeat;
    
    // Replication state
    OperationLog operationLog;
    ReplicationProtocol replicationProtocol;
    uint64_t lastAppliedOperationId;
    std::queue<Operation> pendingOperations;
    
    // Threading
    std::thread replicationThread;
    std::thread heartbeatThread;
    std::atomic<bool> isReceivingReplication;
    
    // Synchronization
    mutable std::mutex replicationMutex;
    mutable std::mutex primaryMutex;
    
    // Configuration
    int serverId;
    std::chrono::milliseconds heartbeatInterval;
    std::chrono::milliseconds connectionTimeout;
    
    // Connection management
    bool establishPrimaryConnection();
    void maintainPrimaryConnection();
    void handlePrimaryDisconnection();
    
    // Replication processing
    void processIncomingReplication();
    void handleReplicationMessage();
    bool validateOperation(const Operation& op);
    void updateLastAppliedOperation(uint64_t operationId);
    
    // Heartbeat management
    void sendHeartbeatToPrimary();
    void processHeartbeatFromPrimary();
    
    // File system operations
    Result applyFileUpload(const Operation& op);
    Result applyFileDelete(const Operation& op);
    
    // Error handling
    void handleReplicationError(const std::string& error);
    void requestResynchronization();
    
    // Helper methods
    bool isValidReplicationMessage(const ReplicationMessage& msg) const;
    void logReplicationActivity(const std::string& activity);
};

#endif // BACKUP_SERVER_H 