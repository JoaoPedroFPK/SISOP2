#ifndef CLUSTER_CONFIG_H
#define CLUSTER_CONFIG_H

#include "replication_protocol.h"
#include <string>
#include <vector>
#include <map>
#include <fstream>

struct ClusterConfig {
    int totalServers;
    int replicationFactor;
    std::vector<ServerInfo> servers;
    int frontEndPort;
    std::string frontEndAddress;
    
    // Timeouts and intervals (in milliseconds)
    int heartbeatInterval;
    int electionTimeout;
    int replicationTimeout;
    int connectionTimeout;
    
    ClusterConfig() 
        : totalServers(1), replicationFactor(1), frontEndPort(8000), 
          frontEndAddress("127.0.0.1"), heartbeatInterval(2000), 
          electionTimeout(5000), replicationTimeout(10000), connectionTimeout(5000) {}
};

class ClusterConfigManager {
public:
    ClusterConfigManager();
    
    // Configuration file operations
    bool loadConfig(const std::string& configFile);
    bool saveConfig(const std::string& configFile, const ClusterConfig& config);
    
    // Configuration access
    const ClusterConfig& getConfig() const { return config; }
    ClusterConfig& getConfig() { return config; }
    
    // Server management
    ServerInfo getServerById(int serverId) const;
    std::vector<ServerInfo> getAllServers() const;
    std::vector<ServerInfo> getActiveServers() const;
    bool addServer(const ServerInfo& serverInfo);
    bool removeServer(int serverId);
    bool updateServerStatus(int serverId, bool isActive, bool isPrimary = false);
    
    // Validation
    bool validateConfig() const;
    
private:
    ClusterConfig config;
    
    // Helper methods for parsing
    void parseConfigLine(const std::string& line, const std::string& currentSection);
    std::pair<std::string, std::string> parseKeyValue(const std::string& line);
    void parseServerSection(const std::string& line, int serverId);
    
    // Current parsing state
    std::string currentSection;
    ServerInfo currentServer;
};

// Utility functions
std::string generateDefaultConfig(int numServers = 3, int basePort = 8001);
bool createDefaultConfigFile(const std::string& filename, int numServers = 3);

#endif // CLUSTER_CONFIG_H 