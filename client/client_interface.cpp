#include <iostream>

int main() {
    std::string command;
    while (true) {
        std::cout << "> ";
        getline(std::cin, command);
        
        if (command == "exit") break;

        std::cout << "Comando recebido: " << command << std::endl;
        // Implementação dos comandos upload, download, delete, list_server, list_client, get_sync_dir aqui.
    }
    return 0;
}
