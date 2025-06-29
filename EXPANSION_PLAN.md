# System Expansion Plan: Passive Replication and Leader Election

## Overview

This document outlines the steps needed to extend our simplified file synchronization system to implement **Passive Replication** and **Leader Election** as specified in the project requirements.

## Current Architecture Analysis

### ✅ **Strengths of Current System**
- Clean modular design with separated concerns
- Unified protocol layer (`SyncProtocol`)
- Robust error handling with `Result` class
- Well-encapsulated classes (`SyncClient`, `SyncServer`, `ClientManager`, `FileManager`)
- No global state dependencies

### 🔄 **Required Architectural Changes**

The current single-server architecture needs to be expanded to support:
1. **Multiple server instances** (Primary + Backup servers)
2. **Front-end component** (transparent client communication)
3. **Inter-server communication** (replication and leader election)
4. **Failure detection and recovery**

---

## Phase 1: Core Infrastructure Extensions

### 1.1 Enhanced Protocol Layer

**Objective**: Extend communication to support server-to-server operations

**Files to Modify/Create**:
- `common/headers/sync_protocol.h` - Add new command types
- `common/headers/replication_protocol.h` - New protocol for server communication
- `common/src/replication_protocol.cpp` - Implementation

**New Protocol Commands**:
```cpp
enum class ReplicationCommand {
    // Server-to-Server Commands
    HEARTBEAT = 20,
    REPLICATE_FILE = 21,
    REPLICATE_DELETE = 22,
    SYNC_STATE = 23,
    
    // Leader Election Commands
    ELECTION_START = 30,
    ELECTION_OK = 31,
    COORDINATOR = 32,
    
    // Front-end Commands
    SERVER_DISCOVERY = 40,
    PRIMARY_UPDATE = 41,
    HEALTH_CHECK = 42
};
```

### 1.2 Server Instance Management

**Objective**: Create framework for multiple server instances

**New Classes to Create**:
```cpp
// server/headers/server_instance.h
class ServerInstance {
private:
    int serverId;
    std::string address;
    int port;
    bool isPrimary;
    bool isActive;
    time_t lastHeartbeat;
    
public:
    // Server identity and status management
    // Network connectivity handling
    // Health monitoring
};

// server/headers/cluster_manager.h
class ClusterManager {
private:
    std::vector<ServerInstance> serverCluster;
    int currentPrimaryId;
    ReplicationProtocol replicationProtocol;
    
public:
    // Cluster topology management
    // Primary/backup coordination
    // Failure detection
};
```

### 1.3 Configuration Management

**Objective**: Support multi-server configuration

**New Files**:
- `common/headers/cluster_config.h` - Cluster configuration definitions
- `config/cluster.conf` - Configuration file for server cluster

**Configuration Structure**:
```ini
[cluster]
total_servers=3
replication_factor=2

[server_1]
id=1
address=127.0.0.1
port=8001
priority=3

[server_2] 
id=2
address=127.0.0.1
port=8002
priority=2

[server_3]
id=3
address=127.0.0.1
port=8003
priority=1
```

---

## Phase 2: Front-End Implementation

### 2.1 Front-End Server

**Objective**: Create transparent proxy between clients and server cluster

**New Components**:
```cpp
// frontend/headers/frontend_server.h
class FrontEndServer {
private:
    ClusterManager clusterManager;
    SyncProtocol clientProtocol;
    ReplicationProtocol serverProtocol;
    int currentPrimaryServer;
    
public:
    void run(int port);
    void handleClientRequest(int clientSocket);
    void forwardToPrimary(const Message& msg);
    void updatePrimaryServer(int newPrimaryId);
    
private:
    void monitorServerHealth();
    void handlePrimaryFailure();
    Result connectToPrimary();
};

// frontend/headers/request_router.h  
class RequestRouter {
public:
    // Route client requests to appropriate server
    // Handle load balancing for read operations
    // Manage connection pooling
};
```

### 2.2 Client Connection Management

**Objective**: Maintain client connections during server transitions

**Modifications**:
- `client/src/sync_client.cpp` - Connect to front-end instead of direct server
- Handle transparent server failover
- Implement reconnection logic

---

## Phase 3: Passive Replication Implementation

### 3.1 Primary Server Extensions

**Objective**: Implement state propagation to backup servers

**Modifications to `SyncServer`**:
```cpp
// server/headers/primary_server.h
class PrimaryServer : public SyncServer {
private:
    std::vector<BackupConnection> backupServers;
    ReplicationProtocol replicationProtocol;
    
public:
    // Override file operations to include replication
    Result replicateOperation(const Operation& op);
    bool waitForBackupConfirmation(const Operation& op);
    void addBackupServer(const ServerInstance& backup);
    void removeBackupServer(int serverId);
    
private:
    void propagateFileChange(const FileOperation& op);
    void ensureConsistency();
};
```

**Key Implementation Requirements**:
1. **Synchronous Replication**: Primary waits for backup confirmation
2. **Operation Ordering**: Maintain consistent operation sequence
3. **State Synchronization**: Ensure backups have current state

### 3.2 Backup Server Implementation

**Objective**: Receive and apply state changes from primary

**New Component**:
```cpp
// server/headers/backup_server.h
class BackupServer : public SyncServer {
private:
    int primaryServerId;
    ReplicationProtocol replicationProtocol;
    OperationLog operationLog;
    
public:
    void connectToPrimary(const ServerInstance& primary);
    void applyReplication(const ReplicationMessage& msg);
    void synchronizeState();
    
    // Override to reject direct client operations
    Result handleClientCommand(const Message& msg) override;
    
private:
    void processReplicationLog();
    void confirmOperation(uint64_t operationId);
};
```

### 3.3 Operation Logging

**Objective**: Maintain operation history for consistency

**New Component**:
```cpp
// server/headers/operation_log.h
struct Operation {
    uint64_t operationId;
    OperationType type;
    std::string filename;
    std::string username;
    std::vector<uint8_t> data;
    time_t timestamp;
    bool confirmed;
};

class OperationLog {
private:
    std::deque<Operation> operations;
    std::mutex logMutex;
    uint64_t nextOperationId;
    
public:
    uint64_t addOperation(const Operation& op);
    void confirmOperation(uint64_t operationId);
    std::vector<Operation> getUnconfirmedOperations();
    void applyOperation(const Operation& op);
};
```

---

## Phase 4: Leader Election Implementation

### 4.1 Election Algorithm Choice

**Recommendation**: Implement **Bully Algorithm** for simplicity and efficiency

**Advantages**:
- Simpler implementation than Ring Algorithm
- Better fault tolerance
- Faster convergence time
- Works well with priority-based server ranking

### 4.2 Election Manager

**Objective**: Coordinate leader election process

**New Component**:
```cpp
// server/headers/election_manager.h
class ElectionManager {
private:
    int serverId;
    int serverPriority;
    std::vector<ServerInstance> knownServers;
    ElectionState currentState;
    
public:
    void startElection();
    void handleElectionMessage(const ElectionMessage& msg);
    void becomeLeader();
    void acknowledgeNewLeader(int leaderId);
    
private:
    void sendElectionMessage(int targetServerId);
    void sendCoordinatorMessage();
    void notifyFrontEnd(int newLeaderId);
};

enum class ElectionState {
    FOLLOWER,
    CANDIDATE, 
    LEADER,
    ELECTION_IN_PROGRESS
};
```

### 4.3 Failure Detection

**Objective**: Detect primary server failures to trigger elections

**New Component**:
```cpp
// server/headers/failure_detector.h
class FailureDetector {
private:
    std::chrono::seconds heartbeatInterval;
    std::chrono::seconds failureTimeout;
    std::map<int, time_t> lastHeartbeat;
    
public:
    void startMonitoring();
    void recordHeartbeat(int serverId);
    bool isServerFailed(int serverId);
    void onServerFailure(int serverId);
    
private:
    void sendHeartbeat();
    void checkFailures();
};
```

---

## Phase 5: Integration and Testing Framework

### 5.1 Deployment Architecture

**Components**:
```
┌─────────────────┐    ┌─────────────────┐
│   Client 1      │    │   Client 2      │
└─────┬───────────┘    └─────┬───────────┘
      │                      │
      └──────────┬───────────┘
                 │
┌─────────────────▼─────────────────┐
│          Front-End Server         │
└─────────────────┬─────────────────┘
                  │
    ┌─────────────┼─────────────┐
    │             │             │
┌───▼───┐    ┌───▼───┐    ┌───▼───┐
│Primary│    │Backup │    │Backup │
│Server │◄──►│Server │◄──►│Server │
│  (P)  │    │  (B1) │    │  (B2) │
└───────┘    └───────┘    └───────┘
```

### 5.2 Testing Strategy

**Test Scenarios**:
1. **Normal Operation**: Verify replication works correctly
2. **Primary Failure**: Test leader election and failover
3. **Network Partition**: Handle split-brain scenarios
4. **Cascading Failures**: Multiple server failures
5. **Client Transparency**: Ensure seamless operation during transitions

**Test Implementation**:
```cpp
// tests/integration_tests.cpp
class ReplicationTest {
public:
    void testNormalReplication();
    void testPrimaryFailover();
    void testElectionConvergence();
    void testClientTransparency();
    void testDataConsistency();
};
```

---

## Phase 6: Enhanced Build System

### 6.1 Updated Makefile

**New Targets**:
```makefile
# Additional targets for new components
frontend: $(FRONTEND_TARGET)
cluster: server frontend
test: $(TEST_TARGET)

# Multi-server deployment
deploy-cluster: cluster
	./scripts/deploy_cluster.sh

# Testing targets
test-replication: test
	./tests/replication_test

test-election: test  
	./tests/election_test
```

### 6.2 Configuration Scripts

**Deployment Scripts**:
- `scripts/start_cluster.sh` - Start all servers in cluster
- `scripts/stop_cluster.sh` - Graceful cluster shutdown
- `scripts/failover_test.sh` - Simulate primary failure

---

## Implementation Timeline

### **Week 1-2: Infrastructure** 
- [ ] Enhanced protocol layer
- [ ] Basic cluster management
- [ ] Configuration system

### **Week 3-4: Front-End**
- [ ] Front-end server implementation
- [ ] Request routing
- [ ] Client connection management

### **Week 5-6: Replication**
- [ ] Primary server replication logic
- [ ] Backup server implementation  
- [ ] Operation logging system

### **Week 7-8: Leader Election**
- [ ] Bully algorithm implementation
- [ ] Failure detection
- [ ] Election coordination

### **Week 9-10: Integration & Testing**
- [ ] End-to-end testing
- [ ] Performance optimization
- [ ] Documentation completion

---

## Risk Mitigation

### **Technical Risks**:
1. **Split-Brain Scenarios**: Implement quorum-based decisions
2. **Race Conditions**: Careful synchronization in election process  
3. **Data Consistency**: Strong ordering guarantees in replication
4. **Performance Impact**: Optimize replication overhead

### **Mitigation Strategies**:
- Extensive testing with network simulation
- Formal verification of election algorithm
- Performance benchmarking at each phase
- Rollback capability to single-server mode

---

## Success Criteria

### **Functional Requirements**:
- ✅ Transparent client experience during failover
- ✅ Data consistency across all replicas  
- ✅ Automatic leader election on primary failure
- ✅ Support for 2+ backup servers

### **Performance Requirements**:
- ✅ Replication overhead < 50% of base performance
- ✅ Failover time < 30 seconds
- ✅ Election convergence < 10 seconds

### **Reliability Requirements**:
- ✅ No data loss during planned failover
- ✅ Consistent state after network partitions
- ✅ Graceful handling of cascading failures

This expansion plan builds upon our simplified architecture while adding the sophisticated distributed systems capabilities required by the new specifications. 