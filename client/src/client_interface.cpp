#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <unistd.h>
#include <sys/socket.h>
#include "sync.h"

// Command string constants
const std::string CMD_EXIT = "exit";
const std::string CMD_UPLOAD = "upload";
const std::string CMD_DOWNLOAD = "download";
const std::string CMD_DELETE = "delete";
const std::string CMD_LIST_SERVER = "list_server";
const std::string CMD_LIST_CLIENT = "list_client";
const std::string CMD_GET_SYNC_DIR = "get_sync_dir";
const std::string CMD_HELP = "help";

// Function to split a string into tokens
std::vector<std::string> split_command(const std::string& str) {
    std::vector<std::string> tokens;
    std::istringstream iss(str);
    std::string token;
    
    while (iss >> token) {
        tokens.push_back(token);
    }
    
    return tokens;
}

// Print help information
void print_help() {
    std::cout << "Comandos disponíveis:" << std::endl;
    std::cout << "  " << CMD_UPLOAD << " <path/filename.ext> - Envia um arquivo para o servidor" << std::endl;
    std::cout << "  " << CMD_DOWNLOAD << " <filename.ext> - Baixa um arquivo do servidor para o diretório local" << std::endl;
    std::cout << "  " << CMD_DELETE << " <filename.ext> - Remove um arquivo do diretório de sincronização" << std::endl;
    std::cout << "  " << CMD_LIST_SERVER << " - Lista os arquivos no servidor" << std::endl;
    std::cout << "  " << CMD_LIST_CLIENT << " - Lista os arquivos no diretório de sincronização local" << std::endl;
    std::cout << "  " << CMD_GET_SYNC_DIR << " - Inicializa o diretório de sincronização" << std::endl;
    std::cout << "  " << CMD_EXIT << " - Encerra a sessão com o servidor" << std::endl;
    std::cout << "  " << CMD_HELP << " - Exibe esta ajuda" << std::endl;
}

// Process a single command
bool process_command(const std::string& command) {
    if (command.empty()) {
        return true;
    }
    
    std::vector<std::string> tokens = split_command(command);
    if (tokens.empty()) {
        return true;
    }
    
    std::string cmd = tokens[0];
    
    if (cmd == CMD_EXIT) {
        // Send exit packet and close connection
        packet exit_pkt;
        exit_pkt.type = 10; // CMD_EXIT
        exit_pkt.seqn = 0;
        exit_pkt.total_size = 0;
        exit_pkt.length = 0;
        
        send(server_socket, &exit_pkt, sizeof(packet), 0);
        return false; // Exit command loop
    } 
    else if (cmd == CMD_UPLOAD) {
        if (tokens.size() < 2) {
            std::cout << "Uso: " << CMD_UPLOAD << " <path/filename.ext>" << std::endl;
            return true;
        }
        
        upload_file(tokens[1]);
    }
    else if (cmd == CMD_DOWNLOAD) {
        if (tokens.size() < 2) {
            std::cout << "Uso: " << CMD_DOWNLOAD << " <filename.ext>" << std::endl;
            return true;
        }
        
        download_file(tokens[1]);
    }
    else if (cmd == CMD_DELETE) {
        if (tokens.size() < 2) {
            std::cout << "Uso: " << CMD_DELETE << " <filename.ext>" << std::endl;
            return true;
        }
        
        delete_file(tokens[1]);
    }
    else if (cmd == CMD_LIST_SERVER) {
        list_server_files();
    }
    else if (cmd == CMD_LIST_CLIENT) {
        list_client_files();
    }
    else if (cmd == CMD_GET_SYNC_DIR) {
        get_sync_dir();
    }
    else if (cmd == CMD_HELP) {
        print_help();
    }
    else {
        std::cout << "Comando desconhecido: " << cmd << std::endl;
        print_help();
    }
    
    return true; // Continue command loop
}
