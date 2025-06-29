#include "primary_server.h"
#include "socket_utils.h"
#include <iostream>
#include <algorithm>
#include <unistd.h>

PrimaryServer::PrimaryServer() 
    : SyncServer(), isReplicating(false), 
      replicationFactor(1), replicationTimeout(5000) {}

PrimaryServer::~PrimaryServer() {
    stop();
}

bool PrimaryServer::initializeAsPrimary(const ClusterConfig& config, int serverId) {
    // Initialize base server
    if (!initialize(config.frontEndPort)) {
        return false;
    }
    
    replicationFactor = config.replicationFactor;
    replicationTimeout = std::chrono::milliseconds(config.replicationTimeout);
    
    // Set required confirmations based on replication factor
    operationLog.setRequiredConfirmations(replicationFactor);
    
    std::cout << "Primary server " << serverId << " initialized with replication factor " 
              << replicationFactor << std::endl;
    
    return true;
}

void PrimaryServer::start() {
    SyncServer::start();
    
    isReplicating = true;
    replicationThread = std::thread(&PrimaryServer::processReplicationQueue, this);
    
    std::cout << "Primary server started with replication enabled" << std::endl;
}

void PrimaryServer::stop() {
    isReplicating = false;
    
    if (replicationThread.joinable()) {
        replicationThread.join();
    }
    
    // Disconnect from all backups
    {
        std::lock_guard<std::mutex> lock(backupMutex);
        for (auto& backup : backupServers) {
            disconnectFromBackup(backup);
        }
        backupServers.clear();
    }
    
    SyncServer::stop();
    std::cout << "Primary server stopped" << std::endl;
}

void PrimaryServer::addBackupServer(const ServerInfo& backup) {
    std::lock_guard<std::mutex> lock(backupMutex);
    
    // Check if backup already exists
    auto it = std::find_if(backupServers.begin(), backupServers.end(),
                          [&backup](const BackupConnection& conn) {
                              return conn.serverId == backup.serverId;
                          });
    
    if (it == backupServers.end()) {
        BackupConnection newBackup(backup.serverId, backup.address, backup.port);
        backupServers.push_back(newBackup);
        
        std::cout << "Added backup server " << backup.serverId 
                  << " at " << backup.address << ":" << backup.port << std::endl;
        
        // Try to connect immediately
        connectToBackup(backupServers.back());
        
        // Synchronize the new backup with current state
        synchronizeBackupServer(backup.serverId);
    }
}

void PrimaryServer::removeBackupServer(int serverId) {
    std::lock_guard<std::mutex> lock(backupMutex);
    
    auto it = std::find_if(backupServers.begin(), backupServers.end(),
                          [serverId](const BackupConnection& conn) {
                              return conn.serverId == serverId;
                          });
    
    if (it != backupServers.end()) {
        disconnectFromBackup(*it);
        backupServers.erase(it);
        
        std::cout << "Removed backup server " << serverId << std::endl;
    }
}

std::vector<BackupConnection> PrimaryServer::getActiveBackups() const {
    std::lock_guard<std::mutex> lock(backupMutex);
    std::vector<BackupConnection> active;
    
    std::copy_if(backupServers.begin(), backupServers.end(),
                std::back_inserter(active),
                [](const BackupConnection& conn) { return conn.isConnected; });
    
    return active;
}

Result PrimaryServer::replicateOperation(const Operation& op) {
    // Add operation to log
    uint64_t operationId = operationLog.addOperation(op);
    
    // Get active backups
    std::vector<BackupConnection> activeBackups = getActiveBackups();
    
    if (activeBackups.empty()) {
        // No backups available, operation is immediately confirmed
        operationLog.confirmOperation(operationId, -1);
        return Result(SyncError::SUCCESS);
    }
    
    // Send operation to all active backups
    bool anySucceeded = false;
    {
        std::lock_guard<std::mutex> lock(backupMutex);
        for (auto& backup : backupServers) {
            if (backup.isConnected) {
                if (sendOperationToBackup(op, backup)) {
                    anySucceeded = true;
                } else {
                    handleBackupFailure(backup.serverId);
                }
            }
        }
    }
    
    if (!anySucceeded) {
        return Result(SyncError::CONNECTION_LOST, "Failed to replicate to any backup server");
    }
    
    // Wait for confirmations
    if (waitForBackupConfirmation(operationId, replicationTimeout)) {
        return Result(SyncError::SUCCESS);
    } else {
        return Result(SyncError::TIMEOUT, "Replication confirmation timeout");
    }
}

bool PrimaryServer::waitForBackupConfirmation(uint64_t operationId, std::chrono::milliseconds timeout) {
    auto startTime = std::chrono::steady_clock::now();
    
    while (std::chrono::steady_clock::now() - startTime < timeout) {
        if (operationLog.isOperationConfirmed(operationId)) {
            return true;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    return false;
}

void PrimaryServer::synchronizeBackupServer(int serverId) {
    BackupConnection* backup = findBackupById(serverId);
    if (!backup || !backup->isConnected) {
        return;
    }
    
    std::cout << "Synchronizing backup server " << serverId 
              << " from operation " << backup->lastSyncedOperationId << std::endl;
    
    // Get operations since last sync
    std::vector<Operation> operations = getOperationsSince(backup->lastSyncedOperationId);
    
    // Send operations to backup
    for (const auto& op : operations) {
        if (sendOperationToBackup(op, *backup)) {
            backup->lastSyncedOperationId = op.operationId;
        } else {
            handleBackupFailure(backup->serverId);
            break;
        }
    }
}

Result PrimaryServer::handleUpload(const std::string& username, const std::string& filename, 
                                  const std::vector<uint8_t>& data) {
    // Create operation for logging
    Operation op(0, OperationType::FILE_UPLOAD, filename, username, data.size());
    op.data = data;
    
    // Replicate operation first
    Result replicationResult = replicateOperation(op);
    if (!replicationResult.success()) {
        return replicationResult;
    }
    
    // Then perform the actual upload
    Result uploadResult = SyncServer::handleUpload(username, filename, data);
    
    if (uploadResult.success()) {
        std::cout << "Upload replicated and completed: " << filename 
                  << " (" << data.size() << " bytes)" << std::endl;
    }
    
    return uploadResult;
}

Result PrimaryServer::handleDelete(const std::string& username, const std::string& filename) {
    // Create operation for logging
    Operation op(0, OperationType::FILE_DELETE, filename, username);
    
    // Replicate operation first
    Result replicationResult = replicateOperation(op);
    if (!replicationResult.success()) {
        return replicationResult;
    }
    
    // Then perform the actual deletion
    Result deleteResult = SyncServer::handleDelete(username, filename);
    
    if (deleteResult.success()) {
        std::cout << "Delete replicated and completed: " << filename << std::endl;
    }
    
    return deleteResult;
}

void PrimaryServer::propagateFileChange(const Operation& op) {
    replicateOperation(op);
}

void PrimaryServer::ensureConsistency() {
    // Check for unconfirmed operations and retry
    std::vector<Operation> unconfirmed = operationLog.getUnconfirmedOperations();
    
    for (const auto& op : unconfirmed) {
        std::cout << "Retrying unconfirmed operation " << op.operationId << std::endl;
        replicateOperation(op);
    }
}

bool PrimaryServer::connectToBackup(BackupConnection& backup) {
    backup.sockfd = create_client_socket(backup.address, backup.port);
    
    if (backup.sockfd >= 0) {
        backup.isConnected = true;
        backup.lastHeartbeat = std::chrono::steady_clock::now();
        
        std::cout << "Connected to backup server " << backup.serverId 
                  << " at " << backup.address << ":" << backup.port << std::endl;
        return true;
    } else {
        backup.isConnected = false;
        std::cerr << "Failed to connect to backup server " << backup.serverId << std::endl;
        return false;
    }
}

void PrimaryServer::disconnectFromBackup(BackupConnection& backup) {
    if (backup.sockfd >= 0) {
        close(backup.sockfd);
        backup.sockfd = -1;
    }
    backup.isConnected = false;
}

void PrimaryServer::processReplicationQueue() {
    while (isReplicating) {
        // Monitor backup connections
        monitorBackupConnections();
        
        // Ensure consistency
        ensureConsistency();
        
        // Clean up old operations
        operationLog.cleanupOldOperations();
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

bool PrimaryServer::sendOperationToBackup(const Operation& op, BackupConnection& backup) {
    if (!backup.isConnected) {
        return false;
    }
    
    // Serialize operation
    std::vector<uint8_t> serializedOp = operationLog.serializeOperation(op);
    
    // Send via replication protocol
    Result result = replicationProtocol.sendReplicateFile(backup.sockfd, op.operationId, 
                                                         op.filename, serializedOp);
    
    if (result.success()) {
        backup.lastHeartbeat = std::chrono::steady_clock::now();
        return true;
    } else {
        std::cerr << "Failed to send operation " << op.operationId 
                  << " to backup " << backup.serverId << ": " << result.getMessage() << std::endl;
        return false;
    }
}

void PrimaryServer::handleBackupResponse(int serverId, const Message& response) {
    // This would handle confirmation messages from backup servers
    if (response.command == Command::CONFIRM_OPERATION) {
        uint64_t operationId = std::stoull(response.payload);
        operationLog.confirmOperation(operationId, serverId);
        
        std::cout << "Received confirmation for operation " << operationId 
                  << " from backup server " << serverId << std::endl;
    }
}

void PrimaryServer::sendFullStateTo(BackupConnection& backup) {
    // Send complete state synchronization to a backup server
    std::cout << "Sending full state to backup server " << backup.serverId << std::endl;
    
    // This would involve sending all current files and their metadata
    // For now, we'll just sync operations
    std::vector<Operation> allOps = getOperationsSince(0);
    
    for (const auto& op : allOps) {
        if (!sendOperationToBackup(op, backup)) {
            handleBackupFailure(backup.serverId);
            break;
        }
    }
}

std::vector<Operation> PrimaryServer::getOperationsSince(uint64_t lastOperationId) {
    return operationLog.getOperationsSince(lastOperationId);
}

BackupConnection* PrimaryServer::findBackupById(int serverId) {
    auto it = std::find_if(backupServers.begin(), backupServers.end(),
                          [serverId](const BackupConnection& conn) {
                              return conn.serverId == serverId;
                          });
    
    return (it != backupServers.end()) ? &(*it) : nullptr;
}

bool PrimaryServer::isReplicationRequired(const Operation& op) const {
    // Determine if operation needs replication based on type
    return op.type == OperationType::FILE_UPLOAD || 
           op.type == OperationType::FILE_DELETE;
}

int PrimaryServer::calculateRequiredConfirmations() const {
    std::lock_guard<std::mutex> lock(backupMutex);
    
    int activeBackups = std::count_if(backupServers.begin(), backupServers.end(),
                                     [](const BackupConnection& conn) { 
                                         return conn.isConnected; 
                                     });
    
    // Require confirmation from majority of active backups
    return std::min(replicationFactor, (activeBackups + 1) / 2 + 1);
}

void PrimaryServer::handleBackupFailure(int serverId) {
    std::cout << "Handling failure of backup server " << serverId << std::endl;
    
    BackupConnection* backup = findBackupById(serverId);
    if (backup) {
        disconnectFromBackup(*backup);
        
        // Try to reconnect after a delay
        std::this_thread::sleep_for(std::chrono::seconds(5));
        connectToBackup(*backup);
    }
}

void PrimaryServer::redistributeReplicas() {
    // Logic to redistribute replicas when backup servers fail
    // For now, just ensure we maintain required replication factor
    std::vector<BackupConnection> active = getActiveBackups();
    
    if (static_cast<int>(active.size()) < replicationFactor - 1) {
        std::cout << "Warning: Only " << active.size() << " backup servers available, "
                  << "required: " << (replicationFactor - 1) << std::endl;
    }
}

void PrimaryServer::monitorBackupConnections() {
    std::lock_guard<std::mutex> lock(backupMutex);
    
    auto now = std::chrono::steady_clock::now();
    
    for (auto& backup : backupServers) {
        if (backup.isConnected) {
            auto timeSinceHeartbeat = std::chrono::duration_cast<std::chrono::seconds>(
                now - backup.lastHeartbeat);
            
            if (timeSinceHeartbeat > std::chrono::seconds(30)) {
                std::cout << "Backup server " << backup.serverId << " heartbeat timeout" << std::endl;
                handleBackupFailure(backup.serverId);
            }
        } else {
            // Try to reconnect
            connectToBackup(backup);
        }
    }
} 