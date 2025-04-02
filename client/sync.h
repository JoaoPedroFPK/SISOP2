#ifndef SYNC_H
#define SYNC_H

#include <string>
#include "../common/packet.h"

void sync_start(const char* username, const char* server_ip, int port);
bool sync_dir_exists();
void create_sync_dir();
void watch_sync_dir();
void handle_server_notification(packet& pkt);

// File operations
bool upload_file(const std::string& filepath);
bool download_file(const std::string& filename);
bool delete_file(const std::string& filename);
void list_server_files();
void list_client_files();
void get_sync_dir();

// Global variables
extern int server_socket;
extern std::string sync_dir_path;
extern std::string current_username;

#endif
