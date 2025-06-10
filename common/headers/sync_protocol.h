#ifndef SYNC_PROTOCOL_H
#define SYNC_PROTOCOL_H

#include <string>
#include <vector>
#include <cstdint>
#include "packet.h"

enum class Command {
    LOGIN = 1,
    UPLOAD = 2,
    DOWNLOAD = 3,
    DELETE = 4,
    LIST_SERVER = 5,
    LIST_CLIENT = 6,
    GET_SYNC_DIR = 7,
    DATA_PACKET = 8,
    NOTIFICATION = 9,
    EXIT = 10,
    HEARTBEAT = 20,
    REPLICATE_FILE = 21,
    REPLICATE_DELETE = 22,
    SYNC_STATE = 23,
    CONFIRM_OPERATION = 24,
    REQUEST_STATE = 25,
    ELECTION_START = 30,
    ELECTION_OK = 31,
    COORDINATOR = 32,
    ELECTION_ALIVE = 33,
    SERVER_DISCOVERY = 40,
    PRIMARY_UPDATE = 41,
    HEALTH_CHECK = 42,
    REGISTER_SERVER = 43,
    UNREGISTER_SERVER = 44
};

enum class SyncError {
    SUCCESS,
    CONNECTION_LOST,
    FILE_NOT_FOUND,
    PERMISSION_DENIED,
    PROTOCOL_ERROR,
    TIMEOUT,
    SESSION_LIMIT_REACHED
};

struct Message {
    Command command;
    std::string payload;
    std::vector<uint8_t> data;
    uint32_t sequence;
    uint32_t total_size;
};

class Result {
public:
    Result(SyncError err = SyncError::SUCCESS, const std::string& msg = "") 
        : error(err), message(msg) {}
    
    bool success() const { return error == SyncError::SUCCESS; }
    SyncError getError() const { return error; }
    const std::string& getMessage() const { return message; }
    
private:
    SyncError error;
    std::string message;
};

class SyncProtocol {
public:
    SyncProtocol();
    
    Result sendMessage(int sockfd, const Message& msg);
    Result receiveMessage(int sockfd, Message& msg, uint32_t timeout_ms = 5000);
    Result sendCommand(int sockfd, Command cmd, const std::string& payload = "", 
                      const std::vector<uint8_t>& data = {});
    Result receiveResponse(int sockfd, Message& response, uint32_t timeout_ms = 5000);
    
private:
    uint32_t nextSequence;
    
    packet messageToPacket(const Message& msg);
    Message packetToMessage(const packet& pkt);
    Result sendPacket(int sockfd, const packet& pkt);
    Result receivePacket(int sockfd, packet& pkt, uint32_t timeout_ms);
};

#endif // SYNC_PROTOCOL_H 