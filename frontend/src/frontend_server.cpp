#include "frontend_server.h"
#include "socket_utils.h"
#include <iostream>
#include <algorithm>
#include <unistd.h>

FrontEndServer::FrontEndServer() 
    : currentPrimaryId(-1), isRunning(false), serverSocket(-1), 
      frontEndPort(8000), shouldStop(false) {}

FrontEndServer::~FrontEndServer() {
    stop();
}

bool FrontEndServer::initialize(const std::string& configFile) {
    if (!configManager.loadConfig(configFile)) {
        std::cerr << "Failed to load cluster configuration" << std::endl;
        return false;
    }
    
    const ClusterConfig& config = configManager.getConfig();
    frontEndPort = config.frontEndPort;
    
    // Initialize with configured servers
    for (const auto& server : config.servers) {
        registeredServers[server.serverId] = server;
        if (server.isPrimary) {
            currentPrimaryId = server.serverId;
        }
    }
    
    // If no primary configured, elect based on priority
    if (currentPrimaryId == -1 && !registeredServers.empty()) {
        auto maxPriorityIt = std::max_element(registeredServers.begin(), registeredServers.end(),
                                            [](const auto& a, const auto& b) {
                                                return a.second.priority < b.second.priority;
                                            });
        currentPrimaryId = maxPriorityIt->first;
        registeredServers[currentPrimaryId].isPrimary = true;
    }
    
    std::cout << "Front-end server initialized on port " << frontEndPort 
              << ", primary server: " << currentPrimaryId << std::endl;
    
    return true;
}

void FrontEndServer::run(int port) {
    if (port > 0) {
        frontEndPort = port;
    }
    
    // Create server socket
    serverSocket = create_server_socket(frontEndPort);
    if (serverSocket < 0) {
        std::cerr << "Failed to create front-end server socket" << std::endl;
        return;
    }
    
    isRunning = true;
    shouldStop = false;
    
    std::cout << "Front-end server listening on port " << frontEndPort << std::endl;
    
    // Start health monitoring
    startHealthMonitoring();
    
    // Start accepting connections
    acceptThread = std::thread(&FrontEndServer::acceptConnections, this);
    
    // Wait for stop signal
    if (acceptThread.joinable()) {
        acceptThread.join();
    }
    
    // Clean up
    stopHealthMonitoring();
    
    if (serverSocket >= 0) {
        close(serverSocket);
        serverSocket = -1;
    }
    
    isRunning = false;
}

void FrontEndServer::stop() {
    shouldStop = true;
    
    if (serverSocket >= 0) {
        close(serverSocket);
        serverSocket = -1;
    }
    
    // Wait for threads to finish
    if (acceptThread.joinable()) {
        acceptThread.join();
    }
    
    for (auto& thread : clientThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    clientThreads.clear();
    
    stopHealthMonitoring();
    requestRouter.closeAllConnections();
    
    isRunning = false;
}

void FrontEndServer::registerServer(const ServerInfo& serverInfo) {
    std::lock_guard<std::mutex> lock(serverMapMutex);
    
    registeredServers[serverInfo.serverId] = serverInfo;
    
    if (serverInfo.isPrimary) {
        currentPrimaryId = serverInfo.serverId;
    }
    
    std::cout << "Registered server " << serverInfo.serverId 
              << " at " << serverInfo.address << ":" << serverInfo.port << std::endl;
}

void FrontEndServer::unregisterServer(int serverId) {
    std::lock_guard<std::mutex> lock(serverMapMutex);
    
    auto it = registeredServers.find(serverId);
    if (it != registeredServers.end()) {
        if (it->second.isPrimary) {
            handlePrimaryFailure();
        }
        
        registeredServers.erase(it);
        requestRouter.cleanupConnectionPool(serverId);
        
        std::cout << "Unregistered server " << serverId << std::endl;
    }
}

void FrontEndServer::updatePrimaryServer(int newPrimaryId) {
    std::lock_guard<std::mutex> lock(serverMapMutex);
    
    // Demote current primary
    if (currentPrimaryId != -1) {
        auto it = registeredServers.find(currentPrimaryId);
        if (it != registeredServers.end()) {
            it->second.isPrimary = false;
        }
    }
    
    // Promote new primary
    auto newIt = registeredServers.find(newPrimaryId);
    if (newIt != registeredServers.end()) {
        newIt->second.isPrimary = true;
        currentPrimaryId = newPrimaryId;
        
        std::cout << "Updated primary server to " << newPrimaryId << std::endl;
    }
}

void FrontEndServer::startHealthMonitoring() {
    healthMonitorThread = std::thread(&FrontEndServer::healthMonitorWorker, this);
}

void FrontEndServer::stopHealthMonitoring() {
    if (healthMonitorThread.joinable()) {
        healthMonitorThread.join();
    }
}

void FrontEndServer::acceptConnections() {
    while (!shouldStop && serverSocket >= 0) {
        int clientSocket = accept_connection(serverSocket);
        if (clientSocket >= 0) {
            // Handle client in separate thread
            clientThreads.emplace_back(&FrontEndServer::handleClientConnection, this, clientSocket);
        } else if (!shouldStop) {
            std::cerr << "Accept failed" << std::endl;
            break;
        }
    }
}

void FrontEndServer::handleClientConnection(int clientSocket) {
    while (!shouldStop) {
        Message request;
        Result result = clientProtocol.receiveMessage(clientSocket, request, 5000);
        
        if (!result.success()) {
            if (result.getError() != SyncError::TIMEOUT) {
                std::cerr << "Failed to receive client message: " << result.getMessage() << std::endl;
            }
            break;
        }
        
        processClientRequest(clientSocket, request);
        
        if (request.command == Command::EXIT) {
            break;
        }
    }
    
    close(clientSocket);
}

void FrontEndServer::processClientRequest(int clientSocket, const Message& request) {
    // Check if this is a front-end management command
    switch (request.command) {
        case Command::REGISTER_SERVER:
            handleServerRegistration(clientSocket);
            return;
        case Command::PRIMARY_UPDATE:
            handlePrimaryUpdate(clientSocket);
            return;
        case Command::HEALTH_CHECK:
            handleHealthCheck(clientSocket);
            return;
        default:
            // Regular client request - forward to appropriate server
            break;
    }
    
    // Regular client request - forward to appropriate server
    Message response;
    Result result = forwardToPrimary(request, response);
    
    if (result.success()) {
        clientProtocol.sendMessage(clientSocket, response);
    } else {
        // Send error response
        response.command = request.command;
        response.payload = "ERROR: " + result.getMessage();
        clientProtocol.sendMessage(clientSocket, response);
    }
}

Result FrontEndServer::forwardToPrimary(const Message& msg, Message& response) {
    std::vector<ServerInfo> availableServers = getActiveServers();
    RequestType reqType = requestRouter.classifyRequest(msg);
    
    int targetServerId = requestRouter.selectTargetServer(reqType, availableServers, currentPrimaryId);
    
    if (targetServerId == -1) {
        return Result(SyncError::CONNECTION_LOST, "No available servers");
    }
    
    return forwardToServer(targetServerId, msg, response);
}

Result FrontEndServer::forwardToServer(int serverId, const Message& msg, Message& response) {
    int sockfd = connectToServer(serverId);
    if (sockfd < 0) {
        return Result(SyncError::CONNECTION_LOST, "Cannot connect to server " + std::to_string(serverId));
    }
    
    auto startTime = std::chrono::steady_clock::now();
    
    // Send request
    Result sendResult = clientProtocol.sendMessage(sockfd, msg);
    if (!sendResult.success()) {
        close(sockfd);
        return sendResult;
    }
    
    // Receive response
    Result recvResult = clientProtocol.receiveMessage(sockfd, response, 10000);
    
    auto endTime = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // Record statistics
    RequestType reqType = requestRouter.classifyRequest(msg);
    requestRouter.recordRequest(serverId, reqType);
    requestRouter.recordLatency(serverId, latency);
    
    // Return connection to pool or close
    if (recvResult.success()) {
        requestRouter.returnConnection(serverId, sockfd);
    } else {
        close(sockfd);
    }
    
    return recvResult;
}

int FrontEndServer::connectToPrimaryServer() {
    return connectToServer(currentPrimaryId);
}

int FrontEndServer::connectToServer(int serverId) {
    // Try to get a pooled connection first
    int sockfd = requestRouter.getPooledConnection(serverId);
    if (sockfd >= 0) {
        return sockfd;
    }
    
    // Create new connection
    ServerInfo* server = findServerById(serverId);
    if (!server) {
        return -1;
    }
    
    return create_client_socket(server->address, server->port);
}

void FrontEndServer::disconnectFromServer(int serverId) {
    auto it = serverConnections.find(serverId);
    if (it != serverConnections.end()) {
        close(it->second);
        serverConnections.erase(it);
    }
}

void FrontEndServer::healthMonitorWorker() {
    while (!shouldStop) {
        std::vector<int> serverIds;
        {
            std::lock_guard<std::mutex> lock(serverMapMutex);
            for (const auto& pair : registeredServers) {
                serverIds.push_back(pair.first);
            }
        }
        
        for (int serverId : serverIds) {
            checkServerHealth(serverId);
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void FrontEndServer::checkServerHealth(int serverId) {
    int sockfd = connectToServer(serverId);
    if (sockfd < 0) {
        handleServerFailure(serverId);
        return;
    }
    
    Result result = replicationProtocol.sendHealthCheck(sockfd);
    if (!result.success()) {
        handleServerFailure(serverId);
    }
    
    close(sockfd);
}

void FrontEndServer::handleServerFailure(int serverId) {
    std::cout << "Server " << serverId << " appears to have failed" << std::endl;
    
    {
        std::lock_guard<std::mutex> lock(serverMapMutex);
        auto it = registeredServers.find(serverId);
        if (it != registeredServers.end()) {
            it->second.isActive = false;
        }
    }
    
    if (serverId == currentPrimaryId) {
        handlePrimaryFailure();
    }
}

void FrontEndServer::handlePrimaryFailure() {
    std::cout << "Primary server failed, selecting new primary" << std::endl;
    
    // Find server with highest priority among active servers
    int newPrimaryId = -1;
    int highestPriority = -1;
    
    {
        std::lock_guard<std::mutex> lock(serverMapMutex);
        for (const auto& pair : registeredServers) {
            if (pair.second.isActive && pair.second.priority > highestPriority) {
                highestPriority = pair.second.priority;
                newPrimaryId = pair.first;
            }
        }
    }
    
    if (newPrimaryId != -1) {
        updatePrimaryServer(newPrimaryId);
    }
}

ServerInfo* FrontEndServer::findServerById(int serverId) {
    std::lock_guard<std::mutex> lock(serverMapMutex);
    auto it = registeredServers.find(serverId);
    return (it != registeredServers.end()) ? &it->second : nullptr;
}

ServerInfo FrontEndServer::getPrimaryServer() {
    std::lock_guard<std::mutex> lock(serverMapMutex);
    auto it = registeredServers.find(currentPrimaryId);
    return (it != registeredServers.end()) ? it->second : ServerInfo();
}

std::vector<ServerInfo> FrontEndServer::getActiveServers() {
    std::lock_guard<std::mutex> lock(serverMapMutex);
    std::vector<ServerInfo> activeServers;
    
    for (const auto& pair : registeredServers) {
        if (pair.second.isActive) {
            activeServers.push_back(pair.second);
        }
    }
    
    return activeServers;
}

bool FrontEndServer::isServerHealthy(int serverId) {
    std::lock_guard<std::mutex> lock(serverMapMutex);
    auto it = registeredServers.find(serverId);
    return (it != registeredServers.end()) && it->second.isActive;
}

void FrontEndServer::handleServerRegistration(int socket) {
    ServerInfo serverInfo;
    Result result = replicationProtocol.receiveServerRegistration(socket, serverInfo, 5000);
    
    if (result.success()) {
        registerServer(serverInfo);
        // Send acknowledgment
        replicationProtocol.sendCommand(socket, Command::REGISTER_SERVER, "OK");
    }
}

void FrontEndServer::handlePrimaryUpdate(int socket) {
    int newPrimaryId;
    Result result = replicationProtocol.receivePrimaryUpdate(socket, newPrimaryId, 5000);
    
    if (result.success()) {
        updatePrimaryServer(newPrimaryId);
        // Send acknowledgment
        replicationProtocol.sendCommand(socket, Command::PRIMARY_UPDATE, "OK");
    }
}

void FrontEndServer::handleHealthCheck(int socket) {
    // Respond to health check
    replicationProtocol.sendCommand(socket, Command::HEALTH_CHECK, "OK");
} 