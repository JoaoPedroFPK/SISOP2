#include "sync.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <string>

// Function declarations from client_interface.cpp
void print_help();
std::vector<std::string> split_command(const std::string& str);
bool process_command(const std::string& command);

int main(int argc, char* argv[]) {
    if (argc != 4) {
        printf("Uso: ./client <username> <server_ip_address> <port>\n");
        return 1;
    }

    const char* username = argv[1];
    const char* server_ip = argv[2];
    int port = atoi(argv[3]);

    // Start synchronization in background
    sync_start(username, server_ip, port);
    
    // Now run the command interface
    std::string command;
    print_help();
    
    while (true) {
        std::cout << "> ";
        std::getline(std::cin, command);
        
        if (!process_command(command)) {
            break; // Exit command returned false
        }
    }
    
    return 0;
}
