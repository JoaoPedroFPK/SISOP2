#include "backup_server.h"
#include "socket_utils.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <unistd.h>

BackupServer::BackupServer() 
    : SyncServer(), primarySocket(-1), connectedToPrimary(false), 
      lastAppliedOperationId(0), isReceivingReplication(false), serverId(-1),
      heartbeatInterval(2000), connectionTimeout(10000) {}

BackupServer::~BackupServer() {
    stop();
}

bool BackupServer::initializeAsBackup(const ClusterConfig& config, int serverIdParam) {
    serverId = serverIdParam;
    
    // Find our server configuration
    auto serverIt = std::find_if(config.servers.begin(), config.servers.end(),
                                [serverIdParam](const ServerInfo& server) {
                                    return server.serverId == serverIdParam;
                                });
    
    if (serverIt == config.servers.end()) {
        std::cerr << "Server ID " << serverIdParam << " not found in configuration" << std::endl;
        return false;
    }
    
    // Initialize base server
    if (!initialize(serverIt->port)) {
        return false;
    }
    
    heartbeatInterval = std::chrono::milliseconds(config.heartbeatInterval);
    connectionTimeout = std::chrono::milliseconds(config.connectionTimeout);
    
    std::cout << "Backup server " << serverId << " initialized on port " << serverIt->port << std::endl;
    
    return true;
}

void BackupServer::start() {
    SyncServer::start();
    
    isReceivingReplication = true;
    replicationThread = std::thread(&BackupServer::processIncomingReplication, this);
    heartbeatThread = std::thread(&BackupServer::maintainPrimaryConnection, this);
    
    std::cout << "Backup server " << serverId << " started" << std::endl;
}

void BackupServer::stop() {
    isReceivingReplication = false;
    
    disconnectFromPrimary();
    
    if (replicationThread.joinable()) {
        replicationThread.join();
    }
    
    if (heartbeatThread.joinable()) {
        heartbeatThread.join();
    }
    
    SyncServer::stop();
    std::cout << "Backup server " << serverId << " stopped" << std::endl;
}

void BackupServer::connectToPrimary(const ServerInfo& primary) {
    std::lock_guard<std::mutex> lock(primaryMutex);
    
    primaryServer = primary;
    
    if (establishPrimaryConnection()) {
        std::cout << "Connected to primary server " << primary.serverId 
                  << " at " << primary.address << ":" << primary.port << std::endl;
        
        // Request initial state synchronization
        requestStateSynchronization();
    } else {
        std::cerr << "Failed to connect to primary server " << primary.serverId << std::endl;
    }
}

void BackupServer::disconnectFromPrimary() {
    std::lock_guard<std::mutex> lock(primaryMutex);
    
    if (primarySocket >= 0) {
        close(primarySocket);
        primarySocket = -1;
    }
    
    connectedToPrimary = false;
    std::cout << "Disconnected from primary server" << std::endl;
}

bool BackupServer::isPrimaryConnected() const {
    std::lock_guard<std::mutex> lock(primaryMutex);
    return connectedToPrimary;
}

void BackupServer::applyReplication(const ReplicationMessage& msg) {
    if (!isValidReplicationMessage(msg)) {
        handleReplicationError("Invalid replication message received");
        return;
    }
    
    // Convert to operation
    Operation op(msg.operationId, msg.type, msg.filename, msg.username);
    op.data = msg.data;
    op.timestamp = msg.timestamp;
    
    // Process the operation
    Result result = processReplicationOperation(op);
    
    if (result.success()) {
        updateLastAppliedOperation(msg.operationId);
        sendConfirmationToPrimary(msg.operationId);
        logReplicationActivity("Applied operation " + std::to_string(msg.operationId));
    } else {
        handleReplicationError("Failed to apply operation " + std::to_string(msg.operationId) + 
                              ": " + result.getMessage());
    }
}

void BackupServer::synchronizeState() {
    std::lock_guard<std::mutex> lock(replicationMutex);
    
    std::cout << "Synchronizing state with primary server (last applied: " 
              << lastAppliedOperationId << ")" << std::endl;
    
    // Request operations since our last applied operation
    if (connectedToPrimary) {
        Result result = replicationProtocol.sendRequestState(primarySocket, lastAppliedOperationId);
        if (!result.success()) {
            handleReplicationError("Failed to request state synchronization");
        }
    }
}

Result BackupServer::processReplicationOperation(const Operation& op) {
    std::lock_guard<std::mutex> lock(replicationMutex);
    
    // Validate operation sequence
    if (op.operationId <= lastAppliedOperationId) {
        // Already applied or out of order
        return Result(SyncError::SUCCESS);
    }
    
    if (!validateOperation(op)) {
        return Result(SyncError::PROTOCOL_ERROR, "Invalid operation");
    }
    
    // Apply to file system
    applyOperationToFileSystem(op);
    
    // Add to our operation log
    operationLog.addOperation(op);
    operationLog.confirmOperation(op.operationId, serverId);
    
    return Result(SyncError::SUCCESS);
}

Result BackupServer::handleClientCommand(const Message& msg) {
    // Backup servers should reject direct client operations
    switch (msg.command) {
        case Command::UPLOAD:
        case Command::DELETE:
            return Result(SyncError::PROTOCOL_ERROR, 
                         "Backup server does not accept direct write operations. Connect to primary server.");
        
        case Command::DOWNLOAD:
        case Command::LIST_SERVER:
        case Command::LIST_CLIENT:
        case Command::GET_SYNC_DIR:
            // Allow read operations
            return SyncServer::handleClientCommand(msg);
        
        default:
            return SyncServer::handleClientCommand(msg);
    }
}

void BackupServer::sendConfirmationToPrimary(uint64_t operationId) {
    if (!connectedToPrimary) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(primaryMutex);
    
    Result result = replicationProtocol.sendConfirmOperation(primarySocket, operationId);
    if (!result.success()) {
        handleReplicationError("Failed to send confirmation for operation " + 
                              std::to_string(operationId));
    }
}

void BackupServer::requestStateSynchronization() {
    if (!connectedToPrimary) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(primaryMutex);
    
    Result result = replicationProtocol.sendRequestState(primarySocket, lastAppliedOperationId);
    if (result.success()) {
        std::cout << "Requested state synchronization from operation " 
                  << lastAppliedOperationId << std::endl;
    }
}

void BackupServer::processReplicationQueue() {
    while (isReceivingReplication) {
        std::lock_guard<std::mutex> lock(replicationMutex);
        
        while (!pendingOperations.empty()) {
            Operation op = pendingOperations.front();
            pendingOperations.pop();
            
            Result result = processReplicationOperation(op);
            if (result.success()) {
                sendConfirmationToPrimary(op.operationId);
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void BackupServer::confirmOperation(uint64_t operationId) {
    operationLog.confirmOperation(operationId, serverId);
}

void BackupServer::applyOperationToFileSystem(const Operation& op) {
    switch (op.type) {
        case OperationType::FILE_UPLOAD:
            applyFileUpload(op);
            break;
        case OperationType::FILE_DELETE:
            applyFileDelete(op);
            break;
        default:
            std::cout << "Ignoring operation type " << static_cast<int>(op.type) << std::endl;
            break;
    }
}

bool BackupServer::establishPrimaryConnection() {
    primarySocket = create_client_socket(primaryServer.address, primaryServer.port);
    
    if (primarySocket >= 0) {
        connectedToPrimary = true;
        lastPrimaryHeartbeat = std::chrono::steady_clock::now();
        return true;
    }
    
    return false;
}

void BackupServer::maintainPrimaryConnection() {
    while (isReceivingReplication) {
        if (!connectedToPrimary) {
            // Try to reconnect
            std::lock_guard<std::mutex> lock(primaryMutex);
            if (!primaryServer.address.empty()) {
                establishPrimaryConnection();
            }
        } else {
            // Send heartbeat
            sendHeartbeatToPrimary();
            
            // Check if connection is still alive
            auto now = std::chrono::steady_clock::now();
            auto timeSinceHeartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastPrimaryHeartbeat);
            
            if (timeSinceHeartbeat > connectionTimeout) {
                handlePrimaryDisconnection();
            }
        }
        
        std::this_thread::sleep_for(heartbeatInterval);
    }
}

void BackupServer::handlePrimaryDisconnection() {
    std::cout << "Primary connection lost, attempting to reconnect..." << std::endl;
    disconnectFromPrimary();
}

void BackupServer::processIncomingReplication() {
    while (isReceivingReplication) {
        if (connectedToPrimary) {
            handleReplicationMessage();
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void BackupServer::handleReplicationMessage() {
    Message msg;
    Result result = replicationProtocol.receiveMessage(primarySocket, msg, 1000);
    
    if (result.success()) {
        lastPrimaryHeartbeat = std::chrono::steady_clock::now();
        
        switch (msg.command) {
            case Command::REPLICATE_FILE: {
                // Parse replication message
                ReplicationMessage repMsg;
                // Simple parsing - in production would use proper deserialization
                std::istringstream iss(msg.payload);
                std::string token;
                
                std::getline(iss, token, '|');
                repMsg.operationId = std::stoull(token);
                
                std::getline(iss, token, '|');
                repMsg.type = static_cast<OperationType>(std::stoi(token));
                
                std::getline(iss, repMsg.filename, '|');
                std::getline(iss, repMsg.username, '|');
                
                applyReplication(repMsg);
                break;
            }
            
            case Command::HEARTBEAT:
                processHeartbeatFromPrimary();
                break;
                
            default:
                std::cout << "Received unknown replication command: " 
                          << static_cast<int>(msg.command) << std::endl;
                break;
        }
    }
}

bool BackupServer::validateOperation(const Operation& op) {
    // Basic validation
    if (op.operationId == 0) return false;
    if (op.filename.empty()) return false;
    if (op.username.empty()) return false;
    
    return true;
}

void BackupServer::updateLastAppliedOperation(uint64_t operationId) {
    if (operationId > lastAppliedOperationId) {
        lastAppliedOperationId = operationId;
    }
}

void BackupServer::sendHeartbeatToPrimary() {
    if (!connectedToPrimary) return;
    
    std::lock_guard<std::mutex> lock(primaryMutex);
    
    ServerInfo thisServer;
    thisServer.serverId = serverId;
    thisServer.isActive = true;
    
    Result result = replicationProtocol.sendHeartbeat(primarySocket, thisServer);
    if (!result.success()) {
        handlePrimaryDisconnection();
    }
}

void BackupServer::processHeartbeatFromPrimary() {
    // Update last heartbeat timestamp
    lastPrimaryHeartbeat = std::chrono::steady_clock::now();
}

Result BackupServer::applyFileUpload(const Operation& op) {
    std::cout << "Applying file upload: " << op.filename 
              << " (" << op.data.size() << " bytes)" << std::endl;
    
    // Apply the upload to local file system using base class method
    return SyncServer::handleUpload(op.username, op.filename, op.data);
}

Result BackupServer::applyFileDelete(const Operation& op) {
    std::cout << "Applying file delete: " << op.filename << std::endl;
    
    // Apply the deletion to local file system using base class method
    return SyncServer::handleDelete(op.username, op.filename);
}

void BackupServer::handleReplicationError(const std::string& error) {
    std::cerr << "Replication error: " << error << std::endl;
    
    // Could trigger resynchronization or other recovery actions
    if (connectedToPrimary) {
        requestResynchronization();
    }
}

void BackupServer::requestResynchronization() {
    std::cout << "Requesting full resynchronization with primary" << std::endl;
    
    // Reset our state and request full sync
    lastAppliedOperationId = 0;
    requestStateSynchronization();
}

bool BackupServer::isValidReplicationMessage(const ReplicationMessage& msg) const {
    return msg.operationId > 0 && !msg.filename.empty() && !msg.username.empty();
}

void BackupServer::logReplicationActivity(const std::string& activity) {
    std::cout << "[Backup " << serverId << "] " << activity << std::endl;
} 