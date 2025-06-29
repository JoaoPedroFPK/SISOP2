#include "sync_protocol.h"
#include "socket_utils.h"
#include "common.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>
#include <cstring>
#include <unistd.h>

SyncProtocol::SyncProtocol() : nextSequence(1) {}

Result SyncProtocol::sendMessage(int sockfd, const Message& msg) {
    packet pkt = messageToPacket(msg);
    return sendPacket(sockfd, pkt);
}

Result SyncProtocol::receiveMessage(int sockfd, Message& msg, uint32_t timeout_ms) {
    packet pkt;
    Result result = receivePacket(sockfd, pkt, timeout_ms);
    if (result.success()) {
        msg = packetToMessage(pkt);
    }
    return result;
}

Result SyncProtocol::sendCommand(int sockfd, Command cmd, const std::string& payload, const std::vector<uint8_t>& data) {
    Message msg;
    msg.command = cmd;
    msg.payload = payload;
    msg.data = data;
    msg.sequence = nextSequence++;
    msg.total_size = data.size();
    
    return sendMessage(sockfd, msg);
}

Result SyncProtocol::receiveResponse(int sockfd, Message& response, uint32_t timeout_ms) {
    return receiveMessage(sockfd, response, timeout_ms);
}

packet SyncProtocol::messageToPacket(const Message& msg) {
    packet pkt;
    memset(&pkt, 0, sizeof(packet));
    
    pkt.type = static_cast<uint16_t>(msg.command);
    pkt.seqn = static_cast<uint16_t>(msg.sequence);
    pkt.total_size = msg.total_size;
    
    if (!msg.data.empty()) {
        // For data packets, use binary data
        size_t copySize = std::min(msg.data.size(), sizeof(pkt.payload));
        memcpy(pkt.payload, msg.data.data(), copySize);
        pkt.length = copySize;
    } else {
        // For command packets, use payload string
        size_t copySize = std::min(msg.payload.size(), sizeof(pkt.payload) - 1);
        strncpy(pkt.payload, msg.payload.c_str(), copySize);
        pkt.payload[copySize] = '\0';
        pkt.length = copySize;
    }
    
    return pkt;
}

Message SyncProtocol::packetToMessage(const packet& pkt) {
    Message msg;
    msg.command = static_cast<Command>(pkt.type);
    msg.sequence = pkt.seqn;
    msg.total_size = pkt.total_size;
    
    if (pkt.type == static_cast<uint16_t>(Command::DATA_PACKET)) {
        // For data packets, copy binary data
        msg.data.resize(pkt.length);
        memcpy(msg.data.data(), pkt.payload, pkt.length);
    } else {
        // For command packets, copy as string
        msg.payload = std::string(pkt.payload, pkt.length);
    }
    
    return msg;
}

Result SyncProtocol::sendPacket(int sockfd, const packet& pkt) {
    ssize_t bytes_sent = send(sockfd, &pkt, sizeof(packet), 0);
    if (bytes_sent <= 0) {
        if (errno == ECONNRESET || errno == EPIPE) {
            return Result(SyncError::CONNECTION_LOST, "Connection lost during send");
        }
        return Result(SyncError::PROTOCOL_ERROR, "Failed to send packet: " + std::string(strerror(errno)));
    }
    
    if (bytes_sent != sizeof(packet)) {
        return Result(SyncError::PROTOCOL_ERROR, "Partial packet sent");
    }
    
    return Result(SyncError::SUCCESS);
}

Result SyncProtocol::receivePacket(int sockfd, packet& pkt, uint32_t timeout_ms) {
    // Set up timeout
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    int select_result = select(sockfd + 1, &readfds, NULL, NULL, &tv);
    if (select_result <= 0) {
        if (select_result == 0) {
            return Result(SyncError::TIMEOUT, "Receive timeout");
        }
        return Result(SyncError::PROTOCOL_ERROR, "Select error: " + std::string(strerror(errno)));
    }
    
    memset(&pkt, 0, sizeof(packet));
    ssize_t bytes_received = recv(sockfd, &pkt, sizeof(packet), MSG_WAITALL);
    
    if (bytes_received <= 0) {
        if (errno == ECONNRESET || errno == EPIPE) {
            return Result(SyncError::CONNECTION_LOST, "Connection lost during receive");
        }
        return Result(SyncError::PROTOCOL_ERROR, "Failed to receive packet: " + std::string(strerror(errno)));
    }
    
    if (bytes_received != sizeof(packet)) {
        return Result(SyncError::PROTOCOL_ERROR, "Partial packet received");
    }
    
    return Result(SyncError::SUCCESS);
} 