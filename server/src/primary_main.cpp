#include "primary_server.h"
#include "cluster_config.h"
#include <iostream>
#include <signal.h>

PrimaryServer* primaryServer = nullptr;

void signalHandler(int signal) {
    if (primaryServer) {
        std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
        primaryServer->stop();
    }
}

int main(int argc, char* argv[]) {
    std::cout << "=== File Synchronization Primary Server ===" << std::endl;
    
    // Parse command line arguments
    std::string configFile = "config/cluster.conf";
    int port = 8001; // Default primary port
    int serverId = 1; // Default server ID
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-c" && i + 1 < argc) {
            configFile = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "-i" && i + 1 < argc) {
            serverId = std::stoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -c <file>    Configuration file (default: config/cluster.conf)" << std::endl;
            std::cout << "  -p <port>    Port to listen on (default: 8001)" << std::endl;
            std::cout << "  -i <id>      Server ID (default: 1)" << std::endl;
            std::cout << "  -h, --help   Show this help message" << std::endl;
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
    
    // Create and initialize primary server
    primaryServer = new PrimaryServer();
    
    if (!primaryServer->initializeAsPrimary(configManager.getConfig(), serverId)) {
        std::cerr << "Failed to initialize primary server" << std::endl;
        delete primaryServer;
        return 1;
    }
    
    // Find and add backup servers from configuration
    const ClusterConfig& config = configManager.getConfig();
    for (const auto& server : config.servers) {
        if (server.serverId != serverId && !server.isPrimary) {
            primaryServer->addBackupServer(server);
        }
    }
    
    std::cout << "Starting primary server " << serverId << " on port " << port << "..." << std::endl;
    
    // Run the server (this will block until stopped)
    primaryServer->run(port);
    
    std::cout << "Primary server stopped" << std::endl;
    
    delete primaryServer;
    return 0;
} 