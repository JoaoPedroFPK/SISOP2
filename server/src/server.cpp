#include "connection_handler.h"
#include "replica_manager.h"
#include <iostream>
#include <cstdlib>
#include <vector>
#include <string>

ReplicaConfig g_replica_config;

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Uso:\n"
                  << argv[0] << " primary <porta_primario>\n"
                  << argv[0] << " backup <ip_primario> <porta_primario>\n";
        return 1;
    }

    std::string role = argv[1];
    if (role == "primary" && argc == 3) {
        g_replica_config.role = ReplicaRole::PRIMARY;
        g_replica_config.listen_port = std::atoi(argv[2]);
        // Não precisa de IPs/portas de backups
        g_replica_config.backup_ips = {};
        g_replica_config.backup_ports = {};
    } else if (role == "backup" && argc == 4) {
        g_replica_config.role = ReplicaRole::BACKUP;
        g_replica_config.listen_port = 0; // Não usado para backup
        g_replica_config.backup_ips = {argv[2]};
        g_replica_config.backup_ports = {std::atoi(argv[3])};
    } else {
        std::cout << "Argumentos inválidos.\n";
        return 1;
    }

    start_replica_manager(g_replica_config);
    return 0;
}