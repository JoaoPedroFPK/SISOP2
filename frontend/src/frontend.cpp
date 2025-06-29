#include "frontend_server.h"
#include <iostream>
#include <signal.h>

FrontEndServer* frontendServer = nullptr;

void signalHandler(int signal) {
    if (frontendServer) {
        std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
        frontendServer->stop();
    }
}

int main(int argc, char* argv[]) {
    std::cout << "=== File Synchronization Front-End Server ===" << std::endl;
    
    // Parse command line arguments
    std::string configFile = "config/cluster.conf";
    int port = 0; // Use config file port by default
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-c" && i + 1 < argc) {
            configFile = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -c <file>    Configuration file (default: config/cluster.conf)" << std::endl;
            std::cout << "  -p <port>    Port to listen on (overrides config)" << std::endl;
            std::cout << "  -h, --help   Show this help message" << std::endl;
            return 0;
        }
    }
    
    // Set up signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Create and initialize front-end server
    frontendServer = new FrontEndServer();
    
    if (!frontendServer->initialize(configFile)) {
        std::cerr << "Failed to initialize front-end server" << std::endl;
        delete frontendServer;
        return 1;
    }
    
    std::cout << "Starting front-end server..." << std::endl;
    
    // Run the server (this will block until stopped)
    frontendServer->run(port);
    
    std::cout << "Front-end server stopped" << std::endl;
    
    delete frontendServer;
    return 0;
} 