#ifndef PRIMARY_SERVER_H
#define PRIMARY_SERVER_H

#include "sync_server.h"
#include "operation_log.h"
#include "replication_protocol.h"
#include "cluster_manager.h"
#include <vector>
#include <map>
#include <thread>
#include <atomic>

struct BackupConnection {
    int serverId;
    std::string address;
    int port;
    int sockfd;
    bool isConnected;
    uint64_t lastSyncedOperationId;
    std::chrono::time_point<std::chrono::steady_clock> lastHeartbeat;

    BackupConnection() : serverId(-1), port(0), sockfd(-1), isConnected(false), lastSyncedOperationId(0) {
        lastHeartbeat = std::chrono::steady_clock::now();
    }

    BackupConnection(int id, const std::string& addr, int p)
        : serverId(id), address(addr), port(p), sockfd(-1), isConnected(false), lastSyncedOperationId(0) {
        lastHeartbeat = std::chrono::steady_clock::now();
    }
};

class PrimaryServer : public SyncServer {
public:
    PrimaryServer();
    virtual ~PrimaryServer();

    // Initialization
    bool initializeAsPrimary(const ClusterConfig& config, int serverId);
    void start() override;
    void stop() override;
    virtual void run(int port) override;

    // Backup server management
    void addBackupServer(const ServerInfo& backup);
    void removeBackupServer(int serverId);
    std::vector<BackupConnection> getActiveBackups() const;

    // Replication operations
    Result replicateOperation(const Operation& op);
    bool waitForBackupConfirmation(uint64_t operationId, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
    void synchronizeBackupServer(int serverId);

    // Override base class methods to include replication
    Result handleUpload(const std::string& username, const std::string& filename,
                       const std::vector<uint8_t>& data) override;
    Result handleDelete(const std::string& username, const std::string& filename) override;

    virtual void handleClient(int clientSocket) override;

protected:
    // Replication-specific operations
    void propagateFileChange(const Operation& op);
    void ensureConsistency();

private:

    bool isPrimary;
    // Replication state
    OperationLog operationLog;
    ReplicationProtocol replicationProtocol;
    std::vector<BackupConnection> backupServers;
    mutable std::mutex backupMutex;

    // Threading for replication
    std::thread replicationThread;
    std::atomic<bool> isReplicating;

    // Configuration
    int replicationFactor;
    std::chrono::milliseconds replicationTimeout;

    // Connection management
    bool connectToBackup(BackupConnection& backup);
    void disconnectFromBackup(BackupConnection& backup);
    void monitorBackupConnections();

    // Replication logic
    void processReplicationQueue();
    bool sendOperationToBackup(const Operation& op, BackupConnection& backup);
    void handleBackupResponse(int serverId, const Message& response);

    // State synchronization
    void sendFullStateTo(BackupConnection& backup);
    std::vector<Operation> getOperationsSince(uint64_t lastOperationId);

    // Helper methods
    BackupConnection* findBackupById(int serverId);
    bool isReplicationRequired(const Operation& op) const;
    int calculateRequiredConfirmations() const;

    // Failure handling
    void handleBackupFailure(int serverId);
    void redistributeReplicas();
};

#endif // PRIMARY_SERVER_H