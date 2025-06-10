#include "cluster_config.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <set>

ClusterConfigManager::ClusterConfigManager() : currentSection("") {}

bool ClusterConfigManager::loadConfig(const std::string& configFile) {
    std::ifstream file(configFile);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open config file: " << configFile << std::endl;
        return false;
    }
    
    config = ClusterConfig();  // Reset to defaults
    std::string line;
    
    while (std::getline(file, line)) {
        // Remove leading/trailing whitespace
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        
        // Check for section headers [section_name]
        if (line[0] == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.length() - 2);
            continue;
        }
        
        parseConfigLine(line, currentSection);
    }
    
    file.close();
    return validateConfig();
}

bool ClusterConfigManager::saveConfig(const std::string& configFile, const ClusterConfig& cfg) {
    std::ofstream file(configFile);
    if (!file.is_open()) {
        std::cerr << "Error: Could not create config file: " << configFile << std::endl;
        return false;
    }
    
    // Write cluster section
    file << "[cluster]\n";
    file << "total_servers=" << cfg.totalServers << "\n";
    file << "replication_factor=" << cfg.replicationFactor << "\n";
    file << "frontend_address=" << cfg.frontEndAddress << "\n";
    file << "frontend_port=" << cfg.frontEndPort << "\n";
    file << "heartbeat_interval=" << cfg.heartbeatInterval << "\n";
    file << "election_timeout=" << cfg.electionTimeout << "\n";
    file << "replication_timeout=" << cfg.replicationTimeout << "\n";
    file << "connection_timeout=" << cfg.connectionTimeout << "\n\n";
    
    // Write server sections
    for (const auto& server : cfg.servers) {
        file << "[server_" << server.serverId << "]\n";
        file << "id=" << server.serverId << "\n";
        file << "address=" << server.address << "\n";
        file << "port=" << server.port << "\n";
        file << "priority=" << server.priority << "\n\n";
    }
    
    file.close();
    return true;
}

ServerInfo ClusterConfigManager::getServerById(int serverId) const {
    auto it = std::find_if(config.servers.begin(), config.servers.end(),
                          [serverId](const ServerInfo& server) {
                              return server.serverId == serverId;
                          });
    return (it != config.servers.end()) ? *it : ServerInfo();
}

std::vector<ServerInfo> ClusterConfigManager::getAllServers() const {
    return config.servers;
}

std::vector<ServerInfo> ClusterConfigManager::getActiveServers() const {
    std::vector<ServerInfo> activeServers;
    std::copy_if(config.servers.begin(), config.servers.end(),
                std::back_inserter(activeServers),
                [](const ServerInfo& server) { return server.isActive; });
    return activeServers;
}

bool ClusterConfigManager::addServer(const ServerInfo& serverInfo) {
    // Check if server ID already exists
    auto it = std::find_if(config.servers.begin(), config.servers.end(),
                          [&serverInfo](const ServerInfo& server) {
                              return server.serverId == serverInfo.serverId;
                          });
    
    if (it != config.servers.end()) {
        return false; // Server already exists
    }
    
    config.servers.push_back(serverInfo);
    config.totalServers = config.servers.size();
    return true;
}

bool ClusterConfigManager::removeServer(int serverId) {
    auto it = std::find_if(config.servers.begin(), config.servers.end(),
                          [serverId](const ServerInfo& server) {
                              return server.serverId == serverId;
                          });
    
    if (it == config.servers.end()) {
        return false; // Server not found
    }
    
    config.servers.erase(it);
    config.totalServers = config.servers.size();
    return true;
}

bool ClusterConfigManager::updateServerStatus(int serverId, bool isActive, bool isPrimary) {
    auto it = std::find_if(config.servers.begin(), config.servers.end(),
                          [serverId](const ServerInfo& server) {
                              return server.serverId == serverId;
                          });
    
    if (it == config.servers.end()) {
        return false; // Server not found
    }
    
    it->isActive = isActive;
    it->isPrimary = isPrimary;
    
    // If setting as primary, ensure no other server is primary
    if (isPrimary) {
        for (auto& server : config.servers) {
            if (server.serverId != serverId) {
                server.isPrimary = false;
            }
        }
    }
    
    return true;
}

bool ClusterConfigManager::validateConfig() const {
    if (config.totalServers <= 0) {
        std::cerr << "Error: total_servers must be positive" << std::endl;
        return false;
    }
    
    if (config.replicationFactor <= 0 || config.replicationFactor > config.totalServers) {
        std::cerr << "Error: replication_factor must be between 1 and total_servers" << std::endl;
        return false;
    }
    
    if (config.servers.size() != static_cast<size_t>(config.totalServers)) {
        std::cerr << "Error: Number of server configurations doesn't match total_servers" << std::endl;
        return false;
    }
    
    // Check for duplicate server IDs and ports
    std::set<int> serverIds;
    std::set<int> serverPorts;
    
    for (const auto& server : config.servers) {
        if (serverIds.count(server.serverId)) {
            std::cerr << "Error: Duplicate server ID: " << server.serverId << std::endl;
            return false;
        }
        serverIds.insert(server.serverId);
        
        if (serverPorts.count(server.port)) {
            std::cerr << "Error: Duplicate server port: " << server.port << std::endl;
            return false;
        }
        serverPorts.insert(server.port);
        
        if (server.port <= 0 || server.port > 65535) {
            std::cerr << "Error: Invalid port for server " << server.serverId << std::endl;
            return false;
        }
    }
    
    return true;
}

void ClusterConfigManager::parseConfigLine(const std::string& line, const std::string& section) {
    auto keyValue = parseKeyValue(line);
    if (keyValue.first.empty()) return;
    
    if (section == "cluster") {
        if (keyValue.first == "total_servers") {
            config.totalServers = std::stoi(keyValue.second);
        } else if (keyValue.first == "replication_factor") {
            config.replicationFactor = std::stoi(keyValue.second);
        } else if (keyValue.first == "frontend_address") {
            config.frontEndAddress = keyValue.second;
        } else if (keyValue.first == "frontend_port") {
            config.frontEndPort = std::stoi(keyValue.second);
        } else if (keyValue.first == "heartbeat_interval") {
            config.heartbeatInterval = std::stoi(keyValue.second);
        } else if (keyValue.first == "election_timeout") {
            config.electionTimeout = std::stoi(keyValue.second);
        } else if (keyValue.first == "replication_timeout") {
            config.replicationTimeout = std::stoi(keyValue.second);
        } else if (keyValue.first == "connection_timeout") {
            config.connectionTimeout = std::stoi(keyValue.second);
        }
    } else if (section.substr(0, 7) == "server_") {
        // Extract server ID from section name
        int serverId = std::stoi(section.substr(7));
        parseServerSection(line, serverId);
    }
}

std::pair<std::string, std::string> ClusterConfigManager::parseKeyValue(const std::string& line) {
    size_t pos = line.find('=');
    if (pos == std::string::npos) {
        return {"", ""};
    }
    
    std::string key = line.substr(0, pos);
    std::string value = line.substr(pos + 1);
    
    // Trim whitespace
    key.erase(0, key.find_first_not_of(" \t"));
    key.erase(key.find_last_not_of(" \t") + 1);
    value.erase(0, value.find_first_not_of(" \t"));
    value.erase(value.find_last_not_of(" \t") + 1);
    
    return {key, value};
}

void ClusterConfigManager::parseServerSection(const std::string& line, int serverId) {
    auto keyValue = parseKeyValue(line);
    if (keyValue.first.empty()) return;
    
    // Find or create server info
    auto it = std::find_if(config.servers.begin(), config.servers.end(),
                          [serverId](const ServerInfo& server) {
                              return server.serverId == serverId;
                          });
    
    if (it == config.servers.end()) {
        config.servers.emplace_back();
        it = config.servers.end() - 1;
        it->serverId = serverId;
    }
    
    if (keyValue.first == "id") {
        it->serverId = std::stoi(keyValue.second);
    } else if (keyValue.first == "address") {
        it->address = keyValue.second;
    } else if (keyValue.first == "port") {
        it->port = std::stoi(keyValue.second);
    } else if (keyValue.first == "priority") {
        it->priority = std::stoi(keyValue.second);
    }
}

// Utility functions
std::string generateDefaultConfig(int numServers, int basePort) {
    std::stringstream ss;
    
    ss << "# Default cluster configuration\n";
    ss << "[cluster]\n";
    ss << "total_servers=" << numServers << "\n";
    ss << "replication_factor=" << std::min(2, numServers) << "\n";
    ss << "frontend_address=127.0.0.1\n";
    ss << "frontend_port=8000\n";
    ss << "heartbeat_interval=2000\n";
    ss << "election_timeout=5000\n";
    ss << "replication_timeout=10000\n";
    ss << "connection_timeout=5000\n\n";
    
    for (int i = 1; i <= numServers; i++) {
        ss << "[server_" << i << "]\n";
        ss << "id=" << i << "\n";
        ss << "address=127.0.0.1\n";
        ss << "port=" << (basePort + i - 1) << "\n";
        ss << "priority=" << (numServers - i + 1) << "\n\n";  // Higher ID = lower priority by default
    }
    
    return ss.str();
}

bool createDefaultConfigFile(const std::string& filename, int numServers) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not create config file: " << filename << std::endl;
        return false;
    }
    
    file << generateDefaultConfig(numServers);
    file.close();
    
    std::cout << "Created default configuration file: " << filename << std::endl;
    return true;
} 