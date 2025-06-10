#ifndef REPLICATION_PROTOCOL_H
#define REPLICATION_PROTOCOL_H

#include "sync_protocol.h"
#include <chrono>
#include <map>
#include <vector>

// Server identification and status
struct ServerInfo {
    int serverId;
    std::string address;
    int port;
    int priority;  // Used for leader election (higher = more preferred)
    bool isPrimary;
    bool isActive;
    std::chrono::time_point<std::chrono::steady_clock> lastHeartbeat;
    
    ServerInfo() : serverId(-1), port(0), priority(0), isPrimary(false), isActive(false) {}
    ServerInfo(int id, const std::string& addr, int p, int prio = 0) 
        : serverId(id), address(addr), port(p), priority(prio), isPrimary(false), isActive(true) {
        lastHeartbeat = std::chrono::steady_clock::now();
    }
};

// Operation types for replication
enum class OperationType {
    FILE_UPLOAD,
    FILE_DELETE,
    USER_LOGIN,
    USER_LOGOUT,
    STATE_SYNC
};

// Replication operation structure
struct ReplicationOperation {
    uint64_t operationId;
    OperationType type;
    std::string username;
    std::string filename;
    std::vector<uint8_t> fileData;
    std::chrono::time_point<std::chrono::steady_clock> timestamp;
    bool confirmed;
    
    ReplicationOperation() : operationId(0), confirmed(false) {
        timestamp = std::chrono::steady_clock::now();
    }
};

// Election message types
enum class ElectionMessageType {
    ELECTION,    // Start election
    OK,          // Response to election
    COORDINATOR, // Announce new leader
    ALIVE        // Health check response
};

struct ElectionMessage {
    ElectionMessageType type;
    int senderId;
    int candidateId;  // For coordinator messages
    int priority;
    
    ElectionMessage() : type(ElectionMessageType::ALIVE), senderId(-1), candidateId(-1), priority(0) {}
    ElectionMessage(ElectionMessageType t, int id, int prio = 0) 
        : type(t), senderId(id), candidateId(-1), priority(prio) {}
};

// Enhanced replication protocol class
class ReplicationProtocol : public SyncProtocol {
public:
    ReplicationProtocol();
    
    // Server communication methods
    Result sendHeartbeat(int sockfd, const ServerInfo& serverInfo);
    Result receiveHeartbeat(int sockfd, ServerInfo& serverInfo, uint32_t timeout_ms = 2000);
    
    // Replication methods
    Result sendReplicationOperation(int sockfd, const ReplicationOperation& operation);
    Result receiveReplicationOperation(int sockfd, ReplicationOperation& operation, uint32_t timeout_ms = 5000);
    Result sendOperationConfirmation(int sockfd, uint64_t operationId, bool success);
    Result receiveOperationConfirmation(int sockfd, uint64_t& operationId, bool& success, uint32_t timeout_ms = 5000);
    
    // State synchronization
    Result sendStateRequest(int sockfd, const std::string& username);
    Result receiveStateRequest(int sockfd, std::string& username, uint32_t timeout_ms = 5000);
    Result sendStateData(int sockfd, const std::vector<std::string>& fileList);
    Result receiveStateData(int sockfd, std::vector<std::string>& fileList, uint32_t timeout_ms = 10000);
    
    // Leader election methods
    Result sendElectionMessage(int sockfd, const ElectionMessage& electionMsg);
    Result receiveElectionMessage(int sockfd, ElectionMessage& electionMsg, uint32_t timeout_ms = 3000);
    
    // Front-end communication
    Result sendServerRegistration(int sockfd, const ServerInfo& serverInfo);
    Result receiveServerRegistration(int sockfd, ServerInfo& serverInfo, uint32_t timeout_ms = 5000);
    Result sendPrimaryUpdate(int sockfd, int newPrimaryId);
    Result receivePrimaryUpdate(int sockfd, int& newPrimaryId, uint32_t timeout_ms = 5000);
    Result sendHealthCheck(int sockfd);
    Result receiveHealthCheck(int sockfd, uint32_t timeout_ms = 2000);
    
private:
    // Helper methods for serialization
    std::string serializeServerInfo(const ServerInfo& info);
    ServerInfo deserializeServerInfo(const std::string& data);
    
    std::string serializeReplicationOperation(const ReplicationOperation& operation);
    ReplicationOperation deserializeReplicationOperation(const std::string& data);
    
    std::string serializeElectionMessage(const ElectionMessage& msg);
    ElectionMessage deserializeElectionMessage(const std::string& data);
    
    std::string serializeFileList(const std::vector<std::string>& fileList);
    std::vector<std::string> deserializeFileList(const std::string& data);
    
    // Operation ID management
    uint64_t nextOperationId;
    std::map<uint64_t, ReplicationOperation> pendingOperations;
};

#endif // REPLICATION_PROTOCOL_H 