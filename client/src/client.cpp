#include "sync_client.h"
#include "commands.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <string>
#include <isocline.h> // Include Isocline
#include <arpa/inet.h>
#include <limits>

using namespace std;

// Function declarations
void print_help();
vector<string> split_command(const string& str);
bool process_command(const string& command, SyncClient& client);

// List of available commands for autocompletion
const vector<string> commands = {
    CMD_UPLOAD,
    CMD_DOWNLOAD,
    CMD_DELETE,
    CMD_LIST_SERVER,
    CMD_LIST_CLIENT,
    CMD_GET_SYNC_DIR,
    CMD_EXIT,
    CMD_HELP
};

// Custom completer function
static void word_completer(ic_completion_env_t* cenv, const char* word) {
    for (const auto& cmd : commands) {
        if (cmd.find(word) == 0) {
            ic_add_completion(cenv, cmd.c_str());
        }
    }
}

// Main completer function
static void completer(ic_completion_env_t* cenv, const char* input) {
    ic_complete_filename(cenv, input, 0, ".", NULL);
    ic_complete_word(cenv, input, &word_completer, NULL);
}

int main(int argc, char* argv[]) {
    std::string username;
    std::string server_ip;
    int port = 0;

    auto ask_username = [&]() {
        while (true) {
            std::cout << "Digite o nome de usuário: ";
            std::getline(std::cin, username);
            if (!username.empty()) break;
            std::cout << "Nome de usuário não pode ser vazio.\n";
        }
    };

    auto ask_ip = [&]() {
        while (true) {
            std::cout << "Digite o endereço IP do servidor: ";
            std::getline(std::cin, server_ip);
            struct sockaddr_in sa{};
            if (inet_pton(AF_INET, server_ip.c_str(), &(sa.sin_addr)) == 1) break;
            std::cout << "Endereço IP inválido.\n";
        }
    };

    auto ask_port = [&]() {
        while (true) {
            std::cout << "Digite a porta (1-65535): ";
            if (!(std::cin >> port)) {
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::cout << "Valor inválido.\n";
                continue;
            }
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            if (port > 0 && port <= 65535) break;
            std::cout << "Porta fora do intervalo permitido.\n";
        }
    };

    // Parse provided arguments, prompting for missing/invalid ones
    if (argc >= 2) {
        username = argv[1];
    }
    if (username.empty()) ask_username();

    if (argc >= 3) {
        server_ip = argv[2];
        struct sockaddr_in sa{};
        if (inet_pton(AF_INET, server_ip.c_str(), &(sa.sin_addr)) != 1) {
            std::cout << "Endereço IP fornecido é inválido.\n";
            server_ip.clear();
        }
    }
    if (server_ip.empty()) ask_ip();

    if (argc >= 4) {
        std::string port_str = argv[3];
        try {
            port = std::stoi(port_str);
        } catch (...) {
            port = 0;
        }
        if (port <= 0 || port > 65535) {
            std::cout << "Porta fornecida inválida.\n";
            port = 0;
        }
    }
    if (port == 0) ask_port();

    std::cout << "Conectando como '" << username << "' em " << server_ip << ":" << port << "..." << std::endl;

    // Create and connect sync client
    SyncClient client;
    Result connectResult = client.connect(username, server_ip, port);
    if (!connectResult.success()) {
        std::cerr << "Não foi possível conectar ao servidor: " << connectResult.getMessage() << std::endl;
        return 1;
    }

    // Configure Isocline
    ic_set_history(NULL, -1);
    ic_set_default_completer(&completer, NULL);
    ic_enable_auto_tab(true);

    // Print help information
    print_help();

    // Command loop
    while (client.isConnected()) {
        char* input = ic_readline("");
        if (!input) {
            break; // EOF (Ctrl+D)
        }

        string command(input);
        free(input);

        if (!command.empty()) {
            ic_history_add(command.c_str());
        }

        if (!process_command(command, client)) {
            break; // Exit command
        }
    }

    ic_println("Sessão encerrada.");
    return 0;
}

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
bool process_command(const std::string& command, SyncClient& client) {
    if (command.empty()) {
        return true;
    }

    std::vector<std::string> tokens = split_command(command);
    if (tokens.empty()) {
        return true;
    }

    std::string cmd = tokens[0];

    if (cmd == CMD_EXIT) {
        client.disconnect();
        return false; // Exit command loop
    }
    else if (cmd == CMD_UPLOAD) {
        if (tokens.size() < 2) {
            std::cout << "Uso: " << CMD_UPLOAD << " <path/filename.ext>" << std::endl;
            return true;
        }
        
        Result result = client.uploadFile(tokens[1]);
        if (!result.success()) {
            std::cout << "Erro no upload: " << result.getMessage() << std::endl;
        }
    }
    else if (cmd == CMD_DOWNLOAD) {
        if (tokens.size() < 2) {
            std::cout << "Uso: " << CMD_DOWNLOAD << " <filename.ext>" << std::endl;
            return true;
        }
        
        Result result = client.downloadFile(tokens[1]);
        if (!result.success()) {
            std::cout << "Erro no download: " << result.getMessage() << std::endl;
        }
    }
    else if (cmd == CMD_DELETE) {
        if (tokens.size() < 2) {
            std::cout << "Uso: " << CMD_DELETE << " <filename.ext>" << std::endl;
            return true;
        }
        
        Result result = client.deleteFile(tokens[1]);
        if (!result.success()) {
            std::cout << "Erro ao deletar: " << result.getMessage() << std::endl;
        }
    }
    else if (cmd == CMD_LIST_SERVER) {
        Result result = client.listServerFiles();
        if (!result.success()) {
            std::cout << "Erro ao listar arquivos do servidor: " << result.getMessage() << std::endl;
        }
    }
    else if (cmd == CMD_LIST_CLIENT) {
        Result result = client.listClientFiles();
        if (!result.success()) {
            std::cout << "Erro ao listar arquivos locais: " << result.getMessage() << std::endl;
        }
    }
    else if (cmd == CMD_GET_SYNC_DIR) {
        Result result = client.getSyncDir();
        if (!result.success()) {
            std::cout << "Erro ao sincronizar diretório: " << result.getMessage() << std::endl;
        }
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
