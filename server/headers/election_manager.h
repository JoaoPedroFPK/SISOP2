#ifndef ELECTION_MANAGER_H
#define ELECTION_MANAGER_H

#include "replication_protocol.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

class ClusterManager; // forward declaration

enum class ElectionState {
    FOLLOWER,
    CANDIDATE,
    LEADER
};

class ElectionManager {
public:
    explicit ElectionManager(ClusterManager* cm);
    ~ElectionManager();

    // Start a new election using the Bully algorithm
    void startElection();

    // Query helpers
    bool isLeader() const { return currentState == ElectionState::LEADER; }
    int getLeaderId() const { return currentLeaderId; }

private:
    ClusterManager* clusterManager;
    ReplicationProtocol replicationProtocol;

    std::atomic<ElectionState> currentState;
    int serverId;
    int serverPriority;
    int currentLeaderId;

    std::mutex electionMutex;
    std::condition_variable electionCv;

    // Internal helpers
    int findHighestPriorityAlive();
    void becomeLeader();
};

#endif // ELECTION_MANAGER_H 