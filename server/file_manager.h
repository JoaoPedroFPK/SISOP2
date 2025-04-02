#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include <string>
#include <vector>
#include <sys/stat.h>

struct FileInfo {
    std::string filename;
    time_t mtime;  // modification time
    time_t atime;  // access time
    time_t ctime;  // change time
    size_t size;
};

class FileManager {
public:
    FileManager();
    
    // Initialize user directory
    bool initUserDirectory(const std::string& username);
    
    // List files in user's sync directory
    std::vector<FileInfo> listUserFiles(const std::string& username);
    
    // Upload a file to user's directory
    bool saveFile(const std::string& username, const std::string& filename, 
                 const char* data, size_t size);
    
    // Get file content
    bool getFile(const std::string& username, const std::string& filename, 
                char* buffer, size_t& size);
    
    // Delete a file
    bool deleteFile(const std::string& username, const std::string& filename);
    
    // Check if file exists
    bool fileExists(const std::string& username, const std::string& filename);
    
    // Get file info
    FileInfo getFileInfo(const std::string& username, const std::string& filename);
    
    // Propagate file to all connected devices
    void propagateFileChange(const std::string& username, const std::string& filename, 
                           int excludeSocketFd = -1);

private:
    std::string getUserDir(const std::string& username);
    std::string getFilePath(const std::string& username, const std::string& filename);
};

#endif
