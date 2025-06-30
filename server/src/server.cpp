#include "connection_handler.h"
#include "replica_manager.h"
#include <iostream>
#include <cstdlib>
#include <vector>
#include <string>

ReplicaConfig g_replica_config;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Uso:\n"
                  << argv[0] << " primary <porta_primario> <ip_backup1> <porta_backup1> <ip_backup2> <porta_backup2>\n"
                  << argv[0] << " backup <porta_backup> <ip_primario> <porta_primario>\n";
        return 1;
    }

    std::string role = argv[1];
    if (role == "primary" && argc == 7) {
        g_replica_config.role = ReplicaRole::PRIMARY;
        g_replica_config.listen_port = std::atoi(argv[2]);
        g_replica_config.backup_ips = {argv[3], argv[5]};
        g_replica_config.backup_ports = {std::atoi(argv[4]), std::atoi(argv[6])};
    } else if (role == "backup" && argc == 5) {
        g_replica_config.role = ReplicaRole::BACKUP;
        g_replica_config.listen_port = std::atoi(argv[2]);
        // Para o backup, o IP/porta do primário fica em backup_ips[0]/backup_ports[0]
        g_replica_config.backup_ips = {argv[3]};
        g_replica_config.backup_ports = {std::atoi(argv[4])};
    } else {
        std::cout << "Argumentos inválidos.\n";
        return 1;
    }

    start_replica_manager(g_replica_config);
    return 0;
}