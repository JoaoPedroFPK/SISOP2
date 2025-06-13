#include "election_manager.h"
#include "cluster_manager.h"
#include <iostream>
#include <algorithm>

ElectionManager::ElectionManager(ClusterManager* cm)
    : clusterManager(cm), currentState(ElectionState::FOLLOWER),
      serverId(-1), serverPriority(0), currentLeaderId(-1) {
    // Obtain this server's identity from cluster manager once it is initialized
    if (clusterManager) {
        serverId = clusterManager->getCurrentPrimaryId(); // may be -1 if not known yet
    }
}

ElectionManager::~ElectionManager() {
    // Nothing to clean up yet
}

void ElectionManager::startElection() {
    std::unique_lock<std::mutex> lock(electionMutex);

    if (!clusterManager) return;

    int myId = clusterManager->getThisServerId();
    ServerInfo selfInfo;
    for (const auto& s : clusterManager->getActiveServers()) {
        if (s.serverId == myId) {
            selfInfo = s;
            break;
        }
    }

    if (selfInfo.serverId == -1) {
        // Could not find self info (perhaps configuration issue)
        std::cerr << "[Election] Unable to find self server information in cluster list" << std::endl;
        return;
    }

    serverId = selfInfo.serverId;
    serverPriority = selfInfo.priority;

    std::cout << "[Election] Server " << serverId << " initiating Bully election" << std::endl;

    currentState = ElectionState::CANDIDATE;

    int winnerId = findHighestPriorityAlive();
    if (winnerId == serverId) {
        // We are highest priority alive – become leader
        becomeLeader();
    } else if (winnerId != -1) {
        // A higher-priority server is alive – wait for it to assume leadership
        currentState = ElectionState::FOLLOWER;
        currentLeaderId = winnerId;
        clusterManager->promoteServerToPrimary(winnerId);
        clusterManager->notifyPrimaryChange(winnerId);
        std::cout << "[Election] Deferring to higher-priority server " << winnerId << std::endl;
    } else {
        // No server alive? stay candidate but become leader
        becomeLeader();
    }
}

int ElectionManager::findHighestPriorityAlive() {
    // The Bully algorithm: the highest-priority process alive becomes leader.
    // We query clusterManager for active servers with priority higher than ours.
    std::vector<ServerInfo> activeServers = clusterManager->getActiveServers();

    int bestId = serverId;
    int bestPriority = serverPriority;

    for (const auto& server : activeServers) {
        if (!server.isActive) continue;
        if (server.priority > bestPriority) {
            // If higher priority server is alive, it "bullys" us.
            bestPriority = server.priority;
            bestId = server.serverId;
        }
    }
    return bestId;
}

void ElectionManager::becomeLeader() {
    currentState = ElectionState::LEADER;
    currentLeaderId = serverId;

    std::cout << "[Election] Server " << serverId << " became new primary (leader)" << std::endl;

    // Inform cluster manager and front-end
    clusterManager->promoteServerToPrimary(serverId);
    clusterManager->notifyPrimaryChange(serverId);
} 