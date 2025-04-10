#ifndef SYNC_H
#define SYNC_H

#include <string>
#include <mutex>
#include <map>
#include <condition_variable>
#include "../../common/headers/packet.h"
#include <sys/socket.h>  // For socket constants like SOL_SOCKET

// Forward declarations for socket operations
uint16_t get_next_seq(); 
bool check_socket_status();
packet send_command_and_wait(const packet& cmd);

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
extern std::mutex socket_mutex;
extern std::map<uint16_t, packet> responses;
extern std::mutex responses_mutex;
extern std::condition_variable responses_cv;
extern uint16_t next_seq_number;

// Monitor thread coordination
extern std::mutex monitor_ready_mutex;
extern std::condition_variable monitor_ready_cv;
extern bool monitor_thread_ready;

#endif
