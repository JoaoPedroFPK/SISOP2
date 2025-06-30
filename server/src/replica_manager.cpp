#include "replica_manager.h"
#include "connection_handler.h"
#include "socket_utils.h"
#include "packet.h"
#include "common.h"
#include <thread>
#include <vector>
#include <string>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include "file_manager.h"

static FileManager fileManager;
static pthread_mutex_t fileMutex = PTHREAD_MUTEX_INITIALIZER;

// Função para replicar operações para os backups (usada pelo primário)
/*void replicate_to_backups(const ReplicaConfig& config, const packet& pkt, const std::vector<packet>* data_packets) {
    for (size_t i = 0; i < config.backup_ips.size(); ++i) {
        int sockfd = create_socket();
        if (connect_socket(sockfd, config.backup_ips[i].c_str(), config.backup_ports[i]) == 0) {
            // Envia o comando principal (upload/delete)
            write_all(sockfd, &pkt, sizeof(packet));
            // Se for upload, envie também os data_packets
            if (data_packets) {
                for (const auto& dpkt : *data_packets) {
                    write_all(sockfd, &dpkt, sizeof(packet));
                }
            }
        } else {
            std::cerr << "[ReplicaManager] Falha ao conectar ao backup " << config.backup_ips[i]
                      << ":" << config.backup_ports[i] << std::endl;
        }
        close(sockfd);
    }
}*/

// Loop do primário: inicia o servidor normalmente
void primary_loop(const ReplicaConfig& config) {
    run_server(config.listen_port);
}

void backup_loop(const ReplicaConfig& config) {
    // IP e porta do primário
    std::string primary_ip = config.backup_ips[0];
    int primary_port = config.backup_ports[0];

    while (true) {
        int sockfd = create_socket();
        if (connect_socket(sockfd, primary_ip.c_str(), primary_port) == 0) {
            const char* handshake = "BACKUP";
            write_all(sockfd, handshake, 6);

            std::cout << "[ReplicaManager] Backup conectado ao primário em " << primary_ip << ":" << primary_port << std::endl;
            while (true) {
                packet pkt;
                ssize_t got = read_all(sockfd, &pkt, sizeof(packet));
                if (got == 0) {
                    // Conexão fechada pelo primário
                    continue;
                }
                if (got < 0) {
                    // Timeout ou erro, apenas continue esperando
                    continue;
                }

                if (pkt.type == 2) { // CMD_UPLOAD
                    std::string user_and_file(pkt.payload);
                    size_t sep = user_and_file.find(':');
                    if (sep == std::string::npos) {
                        std::cerr << "[ReplicaManager] Erro: payload sem separador ':'\n";
                        continue;
                    }
                    std::string username = user_and_file.substr(0, sep);
                    std::string filename = user_and_file.substr(sep + 1);

                    // Recebe os data_packets normalmente
                    size_t bytesRead = 0;
                    size_t total_size = pkt.total_size;
                    char* fileData = new char[total_size];

                    while (bytesRead < total_size) {
                        packet dataPkt;
                        ssize_t got_data = read_all(sockfd, &dataPkt, sizeof(packet));
                        if (got_data <= 0) break;
                        memcpy(fileData + bytesRead, dataPkt.payload, dataPkt.length);
                        bytesRead += dataPkt.length;
                    }

                    pthread_mutex_lock(&fileMutex);
                    fileManager.initUserDirectory(username);
                    fileManager.saveFile(username, filename, fileData, bytesRead);
                    pthread_mutex_unlock(&fileMutex);

                    delete[] fileData;
                } else if (pkt.type == 4) { // CMD_DELETE
                    // Espera-se que o payload seja "usuario:arquivo"
                    std::string user_and_file(pkt.payload);
                    size_t sep = user_and_file.find(':');
                    if (sep == std::string::npos) {
                        std::cerr << "[ReplicaManager] Erro: payload sem separador ':' para delete\n";
                        continue;
                    }
                    std::string username = user_and_file.substr(0, sep);
                    std::string filename = user_and_file.substr(sep + 1);

                    printf("[ReplicaManager] Deletando arquivo %s do usuário %s no backup\n", filename.c_str(), username.c_str());

                    pthread_mutex_lock(&fileMutex);
                    fileManager.initUserDirectory(username);
                    bool deleted = fileManager.deleteFile(username, filename);
                    pthread_mutex_unlock(&fileMutex);

                    if (deleted) {
                        std::cout << "[ReplicaManager] Arquivo deletado no backup: " << filename << " do usuário " << username << std::endl;
                    } else {
                        std::cout << "[ReplicaManager] Falha ao deletar arquivo no backup: " << filename << " do usuário " << username << std::endl;
                    }
                }
            }
            close(sockfd);
            std::cerr << "[ReplicaManager] Conexão com primário perdida. Tentando reconectar..." << std::endl;
            sleep(2); // Aguarda antes de tentar reconectar
        } else {
            std::cerr << "[ReplicaManager] Falha ao conectar ao primário. Tentando novamente em 2s..." << std::endl;
            close(sockfd);
            sleep(2);
        }
    }
}

void start_replica_manager(const ReplicaConfig& config) {
    if (config.role == ReplicaRole::PRIMARY) {
        std::cout << "[ReplicaManager] Iniciando como PRIMÁRIO na porta " << config.listen_port << std::endl;
        primary_loop(config);
    } else {
        std::cout << "[ReplicaManager] Iniciando como BACKUP na porta " << config.listen_port << std::endl;
        backup_loop(config);
    }

}