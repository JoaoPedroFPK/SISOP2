#include "replication_protocol.h"
#include <sstream>
#include <cstring>
#include <iomanip>

ReplicationProtocol::ReplicationProtocol() : SyncProtocol(), nextOperationId(1) {}

// Server communication methods
Result ReplicationProtocol::sendHeartbeat(int sockfd, const ServerInfo& serverInfo) {
    std::string payload = serializeServerInfo(serverInfo);
    return sendCommand(sockfd, Command::HEARTBEAT, payload);
}

Result ReplicationProtocol::receiveHeartbeat(int sockfd, ServerInfo& serverInfo, uint32_t timeout_ms) {
    Message msg;
    Result result = receiveMessage(sockfd, msg, timeout_ms);
    if (result.success() && msg.command == Command::HEARTBEAT) {
        serverInfo = deserializeServerInfo(msg.payload);
        serverInfo.lastHeartbeat = std::chrono::steady_clock::now();
    }
    return result;
}

// Replication methods
Result ReplicationProtocol::sendReplicationOperation(int sockfd, const ReplicationOperation& operation) {
    std::string payload = serializeReplicationOperation(operation);
    return sendCommand(sockfd, Command::REPLICATE_FILE, payload, operation.fileData);
}

Result ReplicationProtocol::receiveReplicationOperation(int sockfd, ReplicationOperation& operation, uint32_t timeout_ms) {
    Message msg;
    Result result = receiveMessage(sockfd, msg, timeout_ms);
    if (result.success() && (msg.command == Command::REPLICATE_FILE || msg.command == Command::REPLICATE_DELETE)) {
        operation = deserializeReplicationOperation(msg.payload);
        operation.fileData = msg.data;
    }
    return result;
}

Result ReplicationProtocol::sendOperationConfirmation(int sockfd, uint64_t operationId, bool success) {
    std::stringstream ss;
    ss << operationId << ":" << (success ? "OK" : "FAIL");
    return sendCommand(sockfd, Command::CONFIRM_OPERATION, ss.str());
}

Result ReplicationProtocol::receiveOperationConfirmation(int sockfd, uint64_t& operationId, bool& success, uint32_t timeout_ms) {
    Message msg;
    Result result = receiveMessage(sockfd, msg, timeout_ms);
    if (result.success() && msg.command == Command::CONFIRM_OPERATION) {
        std::stringstream ss(msg.payload);
        std::string status;
        char delimiter;
        ss >> operationId >> delimiter >> status;
        success = (status == "OK");
    }
    return result;
}

// State synchronization
Result ReplicationProtocol::sendStateRequest(int sockfd, const std::string& username) {
    return sendCommand(sockfd, Command::REQUEST_STATE, username);
}

Result ReplicationProtocol::receiveStateRequest(int sockfd, std::string& username, uint32_t timeout_ms) {
    Message msg;
    Result result = receiveMessage(sockfd, msg, timeout_ms);
    if (result.success() && msg.command == Command::REQUEST_STATE) {
        username = msg.payload;
    }
    return result;
}

Result ReplicationProtocol::sendStateData(int sockfd, const std::vector<std::string>& fileList) {
    std::string payload = serializeFileList(fileList);
    return sendCommand(sockfd, Command::SYNC_STATE, payload);
}

Result ReplicationProtocol::receiveStateData(int sockfd, std::vector<std::string>& fileList, uint32_t timeout_ms) {
    Message msg;
    Result result = receiveMessage(sockfd, msg, timeout_ms);
    if (result.success() && msg.command == Command::SYNC_STATE) {
        fileList = deserializeFileList(msg.payload);
    }
    return result;
}

// Leader election methods
Result ReplicationProtocol::sendElectionMessage(int sockfd, const ElectionMessage& electionMsg) {
    std::string payload = serializeElectionMessage(electionMsg);
    Command cmd;
    switch (electionMsg.type) {
        case ElectionMessageType::ELECTION:
            cmd = Command::ELECTION_START;
            break;
        case ElectionMessageType::OK:
            cmd = Command::ELECTION_OK;
            break;
        case ElectionMessageType::COORDINATOR:
            cmd = Command::COORDINATOR;
            break;
        case ElectionMessageType::ALIVE:
            cmd = Command::ELECTION_ALIVE;
            break;
        default:
            return Result(SyncError::PROTOCOL_ERROR, "Unknown election message type");
    }
    return sendCommand(sockfd, cmd, payload);
}

Result ReplicationProtocol::receiveElectionMessage(int sockfd, ElectionMessage& electionMsg, uint32_t timeout_ms) {
    Message msg;
    Result result = receiveMessage(sockfd, msg, timeout_ms);
    if (result.success()) {
        switch (msg.command) {
            case Command::ELECTION_START:
                electionMsg.type = ElectionMessageType::ELECTION;
                break;
            case Command::ELECTION_OK:
                electionMsg.type = ElectionMessageType::OK;
                break;
            case Command::COORDINATOR:
                electionMsg.type = ElectionMessageType::COORDINATOR;
                break;
            case Command::ELECTION_ALIVE:
                electionMsg.type = ElectionMessageType::ALIVE;
                break;
            default:
                return Result(SyncError::PROTOCOL_ERROR, "Invalid election command");
        }
        ElectionMessage tempMsg = deserializeElectionMessage(msg.payload);
        electionMsg.senderId = tempMsg.senderId;
        electionMsg.candidateId = tempMsg.candidateId;
        electionMsg.priority = tempMsg.priority;
    }
    return result;
}

// Front-end communication
Result ReplicationProtocol::sendServerRegistration(int sockfd, const ServerInfo& serverInfo) {
    std::string payload = serializeServerInfo(serverInfo);
    return sendCommand(sockfd, Command::REGISTER_SERVER, payload);
}

Result ReplicationProtocol::receiveServerRegistration(int sockfd, ServerInfo& serverInfo, uint32_t timeout_ms) {
    Message msg;
    Result result = receiveMessage(sockfd, msg, timeout_ms);
    if (result.success() && msg.command == Command::REGISTER_SERVER) {
        serverInfo = deserializeServerInfo(msg.payload);
    }
    return result;
}

Result ReplicationProtocol::sendPrimaryUpdate(int sockfd, int newPrimaryId) {
    return sendCommand(sockfd, Command::PRIMARY_UPDATE, std::to_string(newPrimaryId));
}

Result ReplicationProtocol::receivePrimaryUpdate(int sockfd, int& newPrimaryId, uint32_t timeout_ms) {
    Message msg;
    Result result = receiveMessage(sockfd, msg, timeout_ms);
    if (result.success() && msg.command == Command::PRIMARY_UPDATE) {
        newPrimaryId = std::stoi(msg.payload);
    }
    return result;
}

Result ReplicationProtocol::sendHealthCheck(int sockfd) {
    return sendCommand(sockfd, Command::HEALTH_CHECK);
}

Result ReplicationProtocol::receiveHealthCheck(int sockfd, uint32_t timeout_ms) {
    Message msg;
    Result result = receiveMessage(sockfd, msg, timeout_ms);
    if (result.success() && msg.command == Command::HEALTH_CHECK) {
        return Result(SyncError::SUCCESS);
    }
    return result;
}

// Helper methods for serialization
std::string ReplicationProtocol::serializeServerInfo(const ServerInfo& info) {
    std::stringstream ss;
    ss << info.serverId << "|" << info.address << "|" << info.port << "|" 
       << info.priority << "|" << (info.isPrimary ? "1" : "0") << "|" 
       << (info.isActive ? "1" : "0");
    return ss.str();
}

ServerInfo ReplicationProtocol::deserializeServerInfo(const std::string& data) {
    ServerInfo info;
    std::stringstream ss(data);
    std::string item;
    int index = 0;
    
    while (std::getline(ss, item, '|') && index < 6) {
        switch (index) {
            case 0: info.serverId = std::stoi(item); break;
            case 1: info.address = item; break;
            case 2: info.port = std::stoi(item); break;
            case 3: info.priority = std::stoi(item); break;
            case 4: info.isPrimary = (item == "1"); break;
            case 5: info.isActive = (item == "1"); break;
        }
        index++;
    }
    info.lastHeartbeat = std::chrono::steady_clock::now();
    return info;
}

std::string ReplicationProtocol::serializeReplicationOperation(const ReplicationOperation& operation) {
    std::stringstream ss;
    ss << operation.operationId << "|" << static_cast<int>(operation.type) << "|"
       << operation.username << "|" << operation.filename << "|"
       << (operation.confirmed ? "1" : "0");
    return ss.str();
}

ReplicationOperation ReplicationProtocol::deserializeReplicationOperation(const std::string& data) {
    ReplicationOperation operation;
    std::stringstream ss(data);
    std::string item;
    int index = 0;
    
    while (std::getline(ss, item, '|') && index < 5) {
        switch (index) {
            case 0: operation.operationId = std::stoull(item); break;
            case 1: operation.type = static_cast<OperationType>(std::stoi(item)); break;
            case 2: operation.username = item; break;
            case 3: operation.filename = item; break;
            case 4: operation.confirmed = (item == "1"); break;
        }
        index++;
    }
    operation.timestamp = std::chrono::steady_clock::now();
    return operation;
}

std::string ReplicationProtocol::serializeElectionMessage(const ElectionMessage& msg) {
    std::stringstream ss;
    ss << static_cast<int>(msg.type) << "|" << msg.senderId << "|" 
       << msg.candidateId << "|" << msg.priority;
    return ss.str();
}

ElectionMessage ReplicationProtocol::deserializeElectionMessage(const std::string& data) {
    ElectionMessage msg;
    std::stringstream ss(data);
    std::string item;
    int index = 0;
    
    while (std::getline(ss, item, '|') && index < 4) {
        switch (index) {
            case 0: msg.type = static_cast<ElectionMessageType>(std::stoi(item)); break;
            case 1: msg.senderId = std::stoi(item); break;
            case 2: msg.candidateId = std::stoi(item); break;
            case 3: msg.priority = std::stoi(item); break;
        }
        index++;
    }
    return msg;
}

std::string ReplicationProtocol::serializeFileList(const std::vector<std::string>& fileList) {
    std::stringstream ss;
    for (size_t i = 0; i < fileList.size(); i++) {
        if (i > 0) ss << "|";
        ss << fileList[i];
    }
    return ss.str();
}

std::vector<std::string> ReplicationProtocol::deserializeFileList(const std::string& data) {
    std::vector<std::string> fileList;
    std::stringstream ss(data);
    std::string filename;
    
    while (std::getline(ss, filename, '|')) {
        if (!filename.empty()) {
            fileList.push_back(filename);
        }
    }
    return fileList;
} 