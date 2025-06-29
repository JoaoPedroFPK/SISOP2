#ifndef OPERATION_LOG_H
#define OPERATION_LOG_H

#include <deque>
#include <vector>
#include <string>
#include <mutex>
#include <chrono>
#include <cstdint>
#include "../../common/headers/operation_types.h"  // Use common operation types

struct Operation {
    uint64_t operationId;
    OperationType type;
    std::string filename;
    std::string username;
    std::vector<uint8_t> data;
    size_t fileSize;
    std::chrono::time_point<std::chrono::steady_clock> timestamp;
    bool confirmed;
    int requiredConfirmations;
    int receivedConfirmations;
    
    Operation() : operationId(0), type(OperationType::FILE_UPLOAD), fileSize(0), 
                  confirmed(false), requiredConfirmations(0), receivedConfirmations(0) {
        timestamp = std::chrono::steady_clock::now();
    }
    
    Operation(uint64_t id, OperationType op_type, const std::string& file, 
              const std::string& user, size_t size = 0)
        : operationId(id), type(op_type), filename(file), username(user), 
          fileSize(size), confirmed(false), requiredConfirmations(0), receivedConfirmations(0) {
        timestamp = std::chrono::steady_clock::now();
    }
};

class OperationLog {
public:
    OperationLog();
    ~OperationLog();
    
    // Operation management
    uint64_t addOperation(const Operation& op);
    bool confirmOperation(uint64_t operationId, int serverId);
    void markOperationFailed(uint64_t operationId);
    
    // Query operations
    std::vector<Operation> getUnconfirmedOperations() const;
    std::vector<Operation> getOperationsSince(uint64_t lastOperationId) const;
    Operation* getOperation(uint64_t operationId);
    bool isOperationConfirmed(uint64_t operationId) const;
    
    // State management
    void applyOperation(const Operation& op);
    void setRequiredConfirmations(int count);
    uint64_t getLastOperationId() const;
    
    // Cleanup and maintenance
    void cleanupOldOperations(std::chrono::minutes maxAge = std::chrono::minutes(60));
    size_t getLogSize() const;
    
    // Serialization for network transmission
    std::vector<uint8_t> serializeOperation(const Operation& op) const;
    Operation deserializeOperation(const std::vector<uint8_t>& data) const;
    
private:
    mutable std::mutex logMutex;
    std::deque<Operation> operations;
    uint64_t nextOperationId;
    int defaultRequiredConfirmations;
    
    // Internal helpers
    std::deque<Operation>::iterator findOperation(uint64_t operationId);
    std::deque<Operation>::const_iterator findOperation(uint64_t operationId) const;
    void removeOldOperations(const std::chrono::time_point<std::chrono::steady_clock>& cutoff);
};

#endif // OPERATION_LOG_H 