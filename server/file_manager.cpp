#include "file_manager.h"
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <dirent.h>

namespace fs = std::filesystem;

FileManager::FileManager() {
    // Create main server directory if it doesn't exist
    if (!fs::exists("server_files")) {
        fs::create_directory("server_files");
    }
}

bool FileManager::initUserDirectory(const std::string& username) {
    std::string userDir = getUserDir(username);
    
    // Create user directory if it doesn't exist
    if (!fs::exists(userDir)) {
        return fs::create_directory(userDir);
    }
    
    return true;
}

std::vector<FileInfo> FileManager::listUserFiles(const std::string& username) {
    std::vector<FileInfo> files;
    std::string userDir = getUserDir(username);
    
    if (!fs::exists(userDir)) {
        return files;
    }
    
    for (const auto& entry : fs::directory_iterator(userDir)) {
        if (entry.is_regular_file()) {
            struct stat fileStat;
            std::string filepath = entry.path().string();
            
            if (stat(filepath.c_str(), &fileStat) == 0) {
                FileInfo info;
                info.filename = entry.path().filename().string();
                info.mtime = fileStat.st_mtime;
                info.atime = fileStat.st_atime;
                info.ctime = fileStat.st_ctime;
                info.size = fileStat.st_size;
                files.push_back(info);
            }
        }
    }
    
    return files;
}

bool FileManager::saveFile(const std::string& username, const std::string& filename, 
                         const char* data, size_t size) {
    // Ensure user directory exists
    if (!initUserDirectory(username)) {
        return false;
    }
    
    std::string filepath = getFilePath(username, filename);
    std::ofstream file(filepath, std::ios::binary);
    
    if (!file) {
        return false;
    }
    
    file.write(data, size);
    return file.good();
}

bool FileManager::getFile(const std::string& username, const std::string& filename, 
                        char* buffer, size_t& size) {
    std::string filepath = getFilePath(username, filename);
    
    if (!fs::exists(filepath)) {
        return false;
    }
    
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    if (buffer == nullptr) {
        // Just return the size
        size = fileSize;
        return true;
    }
    
    if (size < fileSize) {
        // Buffer too small
        return false;
    }
    
    file.read(buffer, fileSize);
    size = fileSize;
    
    return file.good();
}

bool FileManager::deleteFile(const std::string& username, const std::string& filename) {
    std::string filepath = getFilePath(username, filename);
    
    if (!fs::exists(filepath)) {
        return false;
    }
    
    return fs::remove(filepath);
}

bool FileManager::fileExists(const std::string& username, const std::string& filename) {
    std::string filepath = getFilePath(username, filename);
    return fs::exists(filepath);
}

FileInfo FileManager::getFileInfo(const std::string& username, const std::string& filename) {
    FileInfo info;
    info.filename = filename;
    
    std::string filepath = getFilePath(username, filename);
    struct stat fileStat;
    
    if (stat(filepath.c_str(), &fileStat) == 0) {
        info.mtime = fileStat.st_mtime;
        info.atime = fileStat.st_atime;
        info.ctime = fileStat.st_ctime;
        info.size = fileStat.st_size;
    }
    
    return info;
}

void FileManager::propagateFileChange(const std::string& username, const std::string& filename, 
                                    int excludeSocketFd) {
    // This function will be implemented later when we have
    // a way to track connected devices for each user
    // It should send updates to all connected sessions except the one that made the change
}

std::string FileManager::getUserDir(const std::string& username) {
    return "server_files/sync_dir_" + username;
}

std::string FileManager::getFilePath(const std::string& username, const std::string& filename) {
    return getUserDir(username) + "/" + filename;
}
