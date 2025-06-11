#include "operation_log.h"
#include <algorithm>
#include <sstream>
#include <iostream>

OperationLog::OperationLog() : nextOperationId(1), defaultRequiredConfirmations(1) {}

OperationLog::~OperationLog() {
    std::lock_guard<std::mutex> lock(logMutex);
    operations.clear();
}

uint64_t OperationLog::addOperation(const Operation& op) {
    std::lock_guard<std::mutex> lock(logMutex);
    
    Operation newOp = op;
    newOp.operationId = nextOperationId++;
    newOp.requiredConfirmations = defaultRequiredConfirmations;
    newOp.receivedConfirmations = 0;
    newOp.confirmed = false;
    
    operations.push_back(newOp);
    
    std::cout << "Added operation " << newOp.operationId << " (" 
              << static_cast<int>(newOp.type) << ") for file: " << newOp.filename 
              << " by user: " << newOp.username << std::endl;
    
    return newOp.operationId;
}

bool OperationLog::confirmOperation(uint64_t operationId, int serverId) {
    std::lock_guard<std::mutex> lock(logMutex);
    
    auto it = findOperation(operationId);
    if (it == operations.end()) {
        return false;
    }
    
    it->receivedConfirmations++;
    
    if (it->receivedConfirmations >= it->requiredConfirmations) {
        it->confirmed = true;
        std::cout << "Operation " << operationId << " confirmed (total confirmations: " 
                  << it->receivedConfirmations << "/" << it->requiredConfirmations << ")" << std::endl;
    }
    
    return it->confirmed;
}

void OperationLog::markOperationFailed(uint64_t operationId) {
    std::lock_guard<std::mutex> lock(logMutex);
    
    auto it = findOperation(operationId);
    if (it != operations.end()) {
        it->confirmed = false;
        it->receivedConfirmations = 0;
        std::cout << "Operation " << operationId << " marked as failed" << std::endl;
    }
}

std::vector<Operation> OperationLog::getUnconfirmedOperations() const {
    std::lock_guard<std::mutex> lock(logMutex);
    std::vector<Operation> unconfirmed;
    
    for (const auto& op : operations) {
        if (!op.confirmed) {
            unconfirmed.push_back(op);
        }
    }
    
    return unconfirmed;
}

std::vector<Operation> OperationLog::getOperationsSince(uint64_t lastOperationId) const {
    std::lock_guard<std::mutex> lock(logMutex);
    std::vector<Operation> recentOps;
    
    for (const auto& op : operations) {
        if (op.operationId > lastOperationId) {
            recentOps.push_back(op);
        }
    }
    
    return recentOps;
}

Operation* OperationLog::getOperation(uint64_t operationId) {
    std::lock_guard<std::mutex> lock(logMutex);
    
    auto it = findOperation(operationId);
    return (it != operations.end()) ? &(*it) : nullptr;
}

bool OperationLog::isOperationConfirmed(uint64_t operationId) const {
    std::lock_guard<std::mutex> lock(logMutex);
    
    auto it = findOperation(operationId);
    return (it != operations.end()) ? it->confirmed : false;
}

void OperationLog::applyOperation(const Operation& op) {
    // This method would contain the actual file system operations
    // For now, we'll just log that the operation was applied
    std::cout << "Applying operation " << op.operationId << " (" 
              << static_cast<int>(op.type) << ") for file: " << op.filename << std::endl;
}

void OperationLog::setRequiredConfirmations(int count) {
    std::lock_guard<std::mutex> lock(logMutex);
    defaultRequiredConfirmations = std::max(1, count);
    
    // Update existing unconfirmed operations
    for (auto& op : operations) {
        if (!op.confirmed) {
            op.requiredConfirmations = defaultRequiredConfirmations;
        }
    }
}

uint64_t OperationLog::getLastOperationId() const {
    std::lock_guard<std::mutex> lock(logMutex);
    return operations.empty() ? 0 : operations.back().operationId;
}

void OperationLog::cleanupOldOperations(std::chrono::minutes maxAge) {
    std::lock_guard<std::mutex> lock(logMutex);
    
    auto cutoff = std::chrono::steady_clock::now() - maxAge;
    removeOldOperations(cutoff);
}

size_t OperationLog::getLogSize() const {
    std::lock_guard<std::mutex> lock(logMutex);
    return operations.size();
}

std::vector<uint8_t> OperationLog::serializeOperation(const Operation& op) const {
    std::vector<uint8_t> serialized;
    
    // Simple serialization - in production would use protobuf or similar
    std::ostringstream oss;
    oss << op.operationId << "|" 
        << static_cast<int>(op.type) << "|"
        << op.filename << "|"
        << op.username << "|"
        << op.fileSize << "|"
        << op.confirmed << "|"
        << op.requiredConfirmations << "|"
        << op.receivedConfirmations;
    
    std::string str = oss.str();
    serialized.assign(str.begin(), str.end());
    
    // Append file data if present
    if (!op.data.empty()) {
        serialized.insert(serialized.end(), op.data.begin(), op.data.end());
    }
    
    return serialized;
}

Operation OperationLog::deserializeOperation(const std::vector<uint8_t>& data) const {
    Operation op;
    
    // Find the delimiter position to separate metadata from file data
    std::string metadata(data.begin(), data.end());
    size_t lastDelim = metadata.find_last_of('|');
    
    if (lastDelim != std::string::npos) {
        std::string metaStr = metadata.substr(0, lastDelim + 1);
        std::istringstream iss(metaStr);
        std::string token;
        
        int field = 0;
        while (std::getline(iss, token, '|')) {
            switch (field++) {
                case 0: op.operationId = std::stoull(token); break;
                case 1: op.type = static_cast<OperationType>(std::stoi(token)); break;
                case 2: op.filename = token; break;
                case 3: op.username = token; break;
                case 4: op.fileSize = std::stoull(token); break;
                case 5: op.confirmed = (token == "1"); break;
                case 6: op.requiredConfirmations = std::stoi(token); break;
                case 7: op.receivedConfirmations = std::stoi(token); break;
            }
        }
        
        // Extract file data if present
        size_t dataStart = metaStr.length();
        if (dataStart < data.size()) {
            op.data.assign(data.begin() + dataStart, data.end());
        }
    }
    
    return op;
}

std::deque<Operation>::iterator OperationLog::findOperation(uint64_t operationId) {
    return std::find_if(operations.begin(), operations.end(),
                       [operationId](const Operation& op) {
                           return op.operationId == operationId;
                       });
}

std::deque<Operation>::const_iterator OperationLog::findOperation(uint64_t operationId) const {
    return std::find_if(operations.begin(), operations.end(),
                       [operationId](const Operation& op) {
                           return op.operationId == operationId;
                       });
}

void OperationLog::removeOldOperations(const std::chrono::time_point<std::chrono::steady_clock>& cutoff) {
    auto it = std::remove_if(operations.begin(), operations.end(),
                            [cutoff](const Operation& op) {
                                return op.confirmed && op.timestamp < cutoff;
                            });
    
    size_t removed = std::distance(it, operations.end());
    operations.erase(it, operations.end());
    
    if (removed > 0) {
        std::cout << "Cleaned up " << removed << " old operations from log" << std::endl;
    }
} 