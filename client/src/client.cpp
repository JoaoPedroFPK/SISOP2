#include "sync.h"
#include "commands.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <string>
#include <isocline.h> // Include Isocline

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
    if (argc != 4) {
        printf("Uso: ./client <username> <server_ip_address> <port>\n");
        return 1;
    }

    const char* username = argv[1];
    const char* server_ip = argv[2];
    int port = atoi(argv[3]);

    // Start synchronization in background
    sync_start(username, server_ip, port);

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
