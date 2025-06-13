#include "backup_server.h"
#include "cluster_config.h"
#include "cluster_manager.h"
#include <iostream>
#include <signal.h>

BackupServer* backupServer = nullptr;
ClusterManager* clusterMgr = nullptr;

void signalHandler(int signal) {
    if (backupServer) {
        std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
        backupServer->stop();
    }
}

int main(int argc, char* argv[]) {
    std::cout << "=== File Synchronization Backup Server ===" << std::endl;
    
    // Parse command line arguments
    std::string configFile = "config/cluster.conf";
    int port = 8002; // Default backup port
    int serverId = 2; // Default server ID
    int primaryId = 1; // Default primary server ID
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-c" && i + 1 < argc) {
            configFile = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "-i" && i + 1 < argc) {
            serverId = std::stoi(argv[++i]);
        } else if (arg == "-primary" && i + 1 < argc) {
            primaryId = std::stoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -c <file>      Configuration file (default: config/cluster.conf)" << std::endl;
            std::cout << "  -p <port>      Port to listen on (default: 8002)" << std::endl;
            std::cout << "  -i <id>        Server ID (default: 2)" << std::endl;
            std::cout << "  -primary <id>  Primary server ID to connect to (default: 1)" << std::endl;
            std::cout << "  -h, --help     Show this help message" << std::endl;
            return 0;
        }
    }
    
    // Set up signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Load configuration
    ClusterConfigManager configManager;
    if (!configManager.loadConfig(configFile)) {
        std::cerr << "Failed to load configuration from " << configFile << std::endl;
        return 1;
    }
    
    // Create cluster manager and initialize
    clusterMgr = new ClusterManager();
    if (!clusterMgr->initialize(configManager.getConfig(), serverId)) {
        std::cerr << "Failed to initialize cluster manager" << std::endl;
        return 1;
    }
    clusterMgr->start();

    // Create and initialize backup server
    backupServer = new BackupServer();
    backupServer->setClusterManager(clusterMgr);
    
    if (!backupServer->initializeAsBackup(configManager.getConfig(), serverId)) {
        std::cerr << "Failed to initialize backup server" << std::endl;
        delete backupServer;
        return 1;
    }
    
    // Find primary server in configuration
    const ClusterConfig& config = configManager.getConfig();
    ServerInfo primaryServer;
    bool foundPrimary = false;
    
    for (const auto& server : config.servers) {
        if (server.serverId == primaryId) {
            primaryServer = server;
            foundPrimary = true;
            break;
        }
    }
    
    if (!foundPrimary) {
        std::cerr << "Primary server " << primaryId << " not found in configuration" << std::endl;
        delete backupServer;
        return 1;
    }
    
    std::cout << "Starting backup server " << serverId << " on port " << port << "..." << std::endl;
    std::cout << "Will connect to primary server " << primaryId 
              << " at " << primaryServer.address << ":" << primaryServer.port << std::endl;
    
    // Start the backup server
    backupServer->start();
    
    // Connect to primary server
    backupServer->connectToPrimary(primaryServer);
    
    // Run the server (this will block until stopped)
    backupServer->run(port);
    
    std::cout << "Backup server stopped" << std::endl;
    
    delete backupServer;
    if (clusterMgr) {
        clusterMgr->stop();
        delete clusterMgr;
    }
    return 0;
} 