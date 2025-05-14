#include "sync.h"
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

// Function declarations from client_interface.cpp
void print_help();
vector<string> split_command(const string& str);
bool process_command(const string& command);

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
        if (cmd.find(word) == 0) { // Suggest commands that start with the input
            ic_add_completion(cenv, cmd.c_str());
        }
    }
}

// Main completer function
static void completer(ic_completion_env_t* cenv, const char* input) {
    // Complete file names (optional, can be removed if not needed)
    ic_complete_filename(cenv, input, 0, ".", NULL);

    // Use custom word completer
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

    const char* username_c = username.c_str();
    const char* server_ip_c = server_ip.c_str();

    std::cout << "Conectando como '" << username << "' em " << server_ip << ":" << port << "..." << std::endl;

    // Start synchronization in background
    sync_start(username_c, server_ip_c, port);

    // Configure Isocline
    ic_set_history(NULL, -1 /* default entries (= 200) */);
    ic_set_default_completer(&completer, NULL); // Set autocompletion callback
    ic_enable_auto_tab(true); // Automatically complete if there's only one match

    // Print help information
    print_help();

    // Command loop
    while (true) {
        // Use Isocline to get input with a prompt
        char* input = ic_readline("");
        if (!input) {
            break; // Exit on EOF (Ctrl+D)
        }

        string command(input);
        free(input); // Free memory allocated by Isocline

        // Add the command to history if it's not empty
        if (!command.empty()) {
            ic_history_add(command.c_str());
        }

        // Process the command
        if (!process_command(command)) {
            break; // Exit command loop
        }
    }

    ic_println("Sessão encerrada.");
    return 0;
}
