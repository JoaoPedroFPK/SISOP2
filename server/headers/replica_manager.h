#pragma once
#include <string>
#include <vector>
#include "packet.h"
#include "common.h"
#include "socket_utils.h"


// Enum para definir o papel do Replica Manager
enum class ReplicaRole {
    PRIMARY,
    BACKUP
};

// Estrutura de configuração do Replica Manager
struct ReplicaConfig {
    ReplicaRole role;                      // PRIMARY ou BACKUP
    int listen_port;                       // Porta para clientes (primário) ou para receber do primário (backup)
    std::vector<std::string> backup_ips;   // Lista de IPs dos backups (usado pelo primário)
    std::vector<int> backup_ports;         // Lista de portas dos backups (usado pelo primário)
};

// Função principal para iniciar o Replica Manager
void start_replica_manager(const ReplicaConfig& config);

// Função utilitária para replicar operações para os backups (usada pelo primário)
void replicate_to_backups(const ReplicaConfig& config, const packet& pkt, const std::vector<packet>* data_packets = nullptr);