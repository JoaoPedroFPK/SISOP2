#include "connection_handler.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>

int main(int argc, char* argv[]) {
    auto ask_port = []() {
        int p = 0;
        while (true) {
            std::cout << "Por favor, introduza a porta (1-65535): ";
            if (!(std::cin >> p)) {
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::cout << "Valor inválido. Tente novamente." << std::endl;
                continue;
            }
            if (p > 0 && p <= 65535) {
                break;
            }
            std::cout << "Porta fora do intervalo permitido. Tente novamente." << std::endl;
        }
        return p;
    };

    int port = 0;
    if (argc == 2) {
        port = std::atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            std::cout << "Porta fornecida inválida." << std::endl;
            port = ask_port();
        }
    } else {
        port = ask_port();
    }

    run_server(port);
    return 0;
}
