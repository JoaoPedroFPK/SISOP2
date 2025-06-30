#include "sync_server.h"
#include "socket_utils.h"
#include "common.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <unistd.h>

SyncServer::SyncServer() : running(false), serverSocket(-1), serverPort(0) {}

SyncServer::~SyncServer() {
    stop();
}

bool SyncServer::initialize(int port) {
    serverPort = port;
    return true;
}

void SyncServer::start() {
    // Default implementation does nothing - derived classes can override
}

void SyncServer::run(int port) {
    if (port > 0) {
        serverPort = port;
    }

    if (!initialize(serverPort)) {
        std::cerr << "Failed to initialize server" << std::endl;
        return;
    }

    start();

    running.store(true);

    serverSocket = create_socket();
    if (serverSocket < 0) {
        std::cerr << "Falha ao criar socket do servidor" << std::endl;
        return;
    }

    if (bind_socket(serverSocket, serverPort) < 0) {
        std::cerr << "Falha ao fazer bind do socket na porta " << serverPort << std::endl;
        close(serverSocket);
        return;
    }

    if (listen_socket(serverSocket) < 0) {
        std::cerr << "Falha ao colocar socket em modo de escuta" << std::endl;
        close(serverSocket);
        return;
    }

    printf("Servidor rodando na porta %d...\n", serverPort);

    while (running.load()) {
        int clientSocket = accept_connection(serverSocket);
        if (clientSocket >= 0) {
            std::thread clientThread(&SyncServer::handleClient, this, clientSocket);
            clientThread.detach();
        }
    }
}

void SyncServer::stop() {
    running.store(false);
    if (serverSocket >= 0) {
        close(serverSocket);
        serverSocket = -1;
    }
}

void SyncServer::handleClient(int clientSocket) {
    printf("Cliente conectado!\n");

    std::string username;

    // Receive login message
    Message loginMsg;
    Result loginResult = protocol.receiveMessage(clientSocket, loginMsg);
    if (!loginResult.success()) {
        printf("Falha ao receber login: %s\n", loginResult.getMessage().c_str());
        close(clientSocket);
        return;
    }

    if (loginMsg.command != Command::LOGIN) {
        printf("Primeiro comando deve ser LOGIN\n");
        close(clientSocket);
        return;
    }

    username = loginMsg.payload;
    printf("Login de usuário: %s\n", username.c_str());

    // Add client to manager
    Result addResult = clientManager.addClient(username, clientSocket);
    if (!addResult.success()) {
        printf("Falha ao adicionar cliente: %s\n", addResult.getMessage().c_str());

        // Send error response
        Message errorMsg;
        errorMsg.command = Command::EXIT;
        errorMsg.payload = addResult.getMessage();
        errorMsg.sequence = loginMsg.sequence;
        protocol.sendMessage(clientSocket, errorMsg);

        close(clientSocket);
        return;
    }

    // Initialize user directory
    fileManager.initUserDirectory(username);

    // Send login response
    Message loginResponse;
    loginResponse.command = Command::LOGIN;
    loginResponse.payload = "OK";
    loginResponse.sequence = loginMsg.sequence;
    loginResponse.total_size = 0;

    Result responseResult = protocol.sendMessage(clientSocket, loginResponse);
    if (!responseResult.success()) {
        printf("Falha ao enviar resposta de login\n");
        clientManager.removeClient(clientSocket);
        close(clientSocket);
        return;
    }

    // Process commands
    while (running.load()) {
        Message msg;
        Result result = protocol.receiveMessage(clientSocket, msg, 5000); // 5 second timeout

        if (!result.success()) {
            if (result.getError() == SyncError::TIMEOUT) {
                continue; // Timeout is expected
            }
            printf("Erro ao receber comando de %s: %s\n", username.c_str(), result.getMessage().c_str());
            break;
        }

        if (msg.command == Command::EXIT) {
            printf("Cliente %s solicitou desconexão\n", username.c_str());
            break;
        }

        processCommand(clientSocket, username, msg);
    }

    clientManager.removeClient(clientSocket);
    close(clientSocket);
    printf("Cliente %s desconectado\n", username.c_str());
}

void SyncServer::processCommand(int sockfd, const std::string& username, const Message& msg) {
    Result result;

    switch (msg.command) {
        case Command::UPLOAD:
            result = handleUploadInternal(sockfd, username, msg);
            break;
        case Command::DOWNLOAD:
            result = handleDownload(sockfd, username, msg);
            break;
        case Command::DELETE:
            result = handleDeleteInternal(sockfd, username, msg);
            break;
        case Command::LIST_SERVER:
            result = handleListServer(sockfd, username);
            break;
        case Command::GET_SYNC_DIR:
            result = handleGetSyncDir(sockfd, username);
            break;
        default:
            printf("Comando não implementado: %d\n", static_cast<int>(msg.command));
            return;
    }

    if (!result.success()) {
        printf("Erro ao processar comando para %s: %s\n",
               username.c_str(), result.getMessage().c_str());
    }
}

Result SyncServer::handleUploadInternal(int sockfd, const std::string& username, const Message& msg) {
    std::string filename = msg.payload;
    printf("Recebendo upload de %s: %s\n", username.c_str(), filename.c_str());

    // Receive file data
    std::vector<uint8_t> fileData;
    Result dataResult = receiveFileData(sockfd, msg.total_size, fileData);
    if (!dataResult.success()) {
        return dataResult;
    }

    // Save file
    bool saveSuccess = fileManager.saveFile(username, filename,
                                           reinterpret_cast<const char*>(fileData.data()),
                                           fileData.size());

    // Send response
    Message response;
    response.command = Command::UPLOAD;
    response.sequence = msg.sequence;
    response.total_size = 0;

    if (saveSuccess) {
        response.payload = "OK";

        // Notify other clients
        clientManager.notifyUserFileChange(username, filename, 'U', sockfd);
    } else {
        response.payload = "SAVE_FAILED";
    }

    return protocol.sendMessage(sockfd, response);
}

Result SyncServer::handleDownload(int sockfd, const std::string& username, const Message& msg) {
    std::string filename = msg.payload;
    printf("Enviando download para %s: %s\n", username.c_str(), filename.c_str());

    if (!fileManager.fileExists(username, filename)) {
        Message response;
        response.command = Command::DOWNLOAD;
        response.sequence = msg.sequence;
        response.payload = "NOT_FOUND";
        response.total_size = 0;
        return protocol.sendMessage(sockfd, response);
    }

    // Get file size first
    size_t fileSize = 0;
    fileManager.getFile(username, filename, nullptr, fileSize);

    // Get file data
    std::vector<uint8_t> fileData(fileSize);
    bool getSuccess = fileManager.getFile(username, filename,
                                         reinterpret_cast<char*>(fileData.data()),
                                         fileSize);

    if (!getSuccess) {
        Message response;
        response.command = Command::DOWNLOAD;
        response.sequence = msg.sequence;
        response.payload = "READ_FAILED";
        response.total_size = 0;
        return protocol.sendMessage(sockfd, response);
    }

    // Send response
    Message response;
    response.command = Command::DOWNLOAD;
    response.sequence = msg.sequence;
    response.payload = "OK";
    response.total_size = fileSize;

    Result responseResult = protocol.sendMessage(sockfd, response);
    if (!responseResult.success()) {
        return responseResult;
    }

    // Send file data
    return sendFileData(sockfd, fileData);
}

Result SyncServer::handleDeleteInternal(int sockfd, const std::string& username, const Message& msg) {
    std::string filename = msg.payload;
    printf("Deletando arquivo de %s: %s\n", username.c_str(), filename.c_str());

    bool deleteSuccess = fileManager.deleteFile(username, filename);

    Message response;
    response.command = Command::DELETE;
    response.sequence = msg.sequence;
    response.total_size = 0;

    if (deleteSuccess) {
        response.payload = "OK";

        // Notify other clients
        clientManager.notifyUserFileChange(username, filename, 'D', sockfd);
    } else {
        response.payload = "NOT_FOUND";
    }

    return protocol.sendMessage(sockfd, response);
}

Result SyncServer::handleListServer(int sockfd, const std::string& username) {
    std::vector<FileInfo> files = fileManager.listUserFiles(username);
    std::string fileList = formatFileList(files);

    Message response;
    response.command = Command::LIST_SERVER;
    response.sequence = 0;
    response.total_size = fileList.size();

    if (fileList.size() <= sizeof(((packet*)0)->payload)) {
        // Fits in one packet
        response.payload = fileList;
        return protocol.sendMessage(sockfd, response);
    } else {
        // Send header first
        response.payload = "";
        Result headerResult = protocol.sendMessage(sockfd, response);
        if (!headerResult.success()) {
            return headerResult;
        }

        // Send data
        std::vector<uint8_t> data(fileList.begin(), fileList.end());
        return sendFileData(sockfd, data);
    }
}

Result SyncServer::handleGetSyncDir(int sockfd, const std::string& username) {
    std::vector<FileInfo> files = fileManager.listUserFiles(username);

    Message response;
    response.command = Command::GET_SYNC_DIR;
    response.sequence = 0;
    response.payload = "OK";
    response.total_size = files.size();

    Result responseResult = protocol.sendMessage(sockfd, response);
    if (!responseResult.success()) {
        return responseResult;
    }

    // Send file notifications
    for (const auto& file : files) {
        Message notification;
        notification.command = Command::NOTIFICATION;
        notification.payload = "U:" + file.filename;
        notification.sequence = 0;
        notification.total_size = 0;

        Result notifResult = protocol.sendMessage(sockfd, notification);
        if (!notifResult.success()) {
            return notifResult;
        }
    }

    return Result(SyncError::SUCCESS);
}

Result SyncServer::sendFileData(int sockfd, const std::vector<uint8_t>& data) {
    size_t offset = 0;
    size_t chunkSize = sizeof(((packet*)0)->payload);

    while (offset < data.size()) {
        size_t currentChunk = std::min(chunkSize, data.size() - offset);

        Message dataMsg;
        dataMsg.command = Command::DATA_PACKET;
        dataMsg.data = std::vector<uint8_t>(data.begin() + offset,
                                           data.begin() + offset + currentChunk);
        dataMsg.sequence = offset / chunkSize + 1;
        dataMsg.total_size = currentChunk;

        Result result = protocol.sendMessage(sockfd, dataMsg);
        if (!result.success()) {
            return result;
        }

        offset += currentChunk;
    }

    return Result(SyncError::SUCCESS);
}

Result SyncServer::receiveFileData(int sockfd, size_t expectedSize, std::vector<uint8_t>& data) {
    data.clear();
    data.reserve(expectedSize);

    while (data.size() < expectedSize) {
        Message dataMsg;
        Result result = protocol.receiveMessage(sockfd, dataMsg);
        if (!result.success()) {
            return result;
        }

        if (dataMsg.command != Command::DATA_PACKET) {
            return Result(SyncError::PROTOCOL_ERROR, "Expected data packet");
        }

        data.insert(data.end(), dataMsg.data.begin(), dataMsg.data.end());
    }

    return Result(SyncError::SUCCESS);
}

std::string SyncServer::formatFileList(const std::vector<FileInfo>& files) {
    std::ostringstream oss;

    for (const auto& file : files) {
        oss << file.filename << ","
            << file.size << ","
            << file.mtime << ","
            << file.atime << ","
            << file.ctime << "\n";
    }

    return oss.str();
}

// Global function for compatibility
void run_server(int port) {
    SyncServer server;
    server.run(port);
}

// Public command handlers for replication
Result SyncServer::handleUpload(const std::string& username, const std::string& filename,
                               const std::vector<uint8_t>& data) {
    // Initialize user directory if needed
    fileManager.initUserDirectory(username);

    // Save file using file manager
    bool saveSuccess = fileManager.saveFile(username, filename,
                                           reinterpret_cast<const char*>(data.data()),
                                           data.size());

    if (saveSuccess) {
        printf("Uploaded file %s for user %s (%zu bytes)\n",
               filename.c_str(), username.c_str(), data.size());
        return Result(SyncError::SUCCESS);
    } else {
        return Result(SyncError::PERMISSION_DENIED, "Failed to save file");
    }
}

Result SyncServer::handleDelete(const std::string& username, const std::string& filename) {
    bool deleteSuccess = fileManager.deleteFile(username, filename);

    if (deleteSuccess) {
        printf("Deleted file %s for user %s\n", filename.c_str(), username.c_str());
        return Result(SyncError::SUCCESS);
    } else {
        return Result(SyncError::FILE_NOT_FOUND, "File not found or delete failed");
    }
}

Result SyncServer::handleClientCommand(const Message& /*msg*/) {
    // Default implementation - derived classes can override
    return Result(SyncError::SUCCESS);
}