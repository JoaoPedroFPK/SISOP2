# File Synchronization System - Simplified Architecture

A clean, modern implementation of a client-server file synchronization system with simplified architecture and improved maintainability.

## Architecture Overview

The system has been completely refactored from the original complex implementation to provide:

- **Unified Protocol Layer**: Consistent communication handling
- **Simplified Client**: Single `SyncClient` class encapsulating all functionality  
- **Clean Server Architecture**: Modular design with separated concerns
- **Robust Error Handling**: Type-safe error management
- **No Global State**: All state properly encapsulated in classes

## Key Improvements

### 🔧 Simplified Design
- Reduced main client file from 1449 lines to ~200 lines
- Eliminated complex global variables and threading issues
- Clean separation of concerns across all components

### 🚀 Better Performance  
- Unified protocol reduces communication overhead
- Simplified threading model reduces contention
- Efficient file transfer with proper chunking

### 🛡️ Enhanced Reliability
- Type-safe error handling with `Result` class
- Automatic connection management and recovery
- Proper resource cleanup and thread management

### 🧪 Improved Maintainability
- Single responsibility principle applied throughout
- Classes can be unit tested in isolation
- Clear interfaces and dependencies

## Building the System

```bash
# Build everything
make all

# Build individual components
make client
make server

# Clean build artifacts
make clean

# Show help
make help
```

## Usage

### Starting the Server
```bash
./server/server [port]
```

### Starting the Client
```bash
./client/client [username] [server_ip] [port]
```

### Available Commands
- `upload <filepath>` - Upload a file to the server
- `download <filename>` - Download a file from the server
- `delete <filename>` - Delete a file from the server
- `list_server` - List files on the server
- `list_client` - List files in local sync directory
- `get_sync_dir` - Synchronize with server files
- `exit` - Disconnect from server
- `help` - Show available commands

## File Structure

```
.
├── client/
│   ├── headers/
│   │   ├── sync_client.h      # Main client class
│   │   └── commands.h         # Command definitions
│   └── src/
│       ├── client.cpp         # Client main and interface
│       └── sync_client.cpp    # Client implementation
├── server/
│   ├── headers/
│   │   ├── sync_server.h      # Main server class
│   │   ├── client_manager.h   # Client session management
│   │   └── file_manager.h     # File operations
│   └── src/
│       ├── server.cpp         # Server main
│       ├── sync_server.cpp    # Server implementation
│       ├── client_manager.cpp # Client management
│       └── file_manager.cpp   # File operations
└── common/
    ├── headers/
    │   ├── sync_protocol.h    # Unified protocol
    │   ├── socket_utils.h     # Socket utilities
    │   ├── packet.h           # Packet structure
    │   └── common.h           # Common definitions
    └── src/
        ├── sync_protocol.cpp  # Protocol implementation
        └── socket_utils.cpp   # Socket utilities
```

## Key Classes

### SyncClient
Encapsulates all client-side functionality:
- Connection management
- File operations (upload/download/delete)
- Real-time synchronization
- Background file monitoring

### SyncServer  
Main server orchestration:
- Client connection handling
- Command processing
- File management coordination

### ClientManager
Handles multiple client sessions:
- Session limit enforcement (2 per user)
- Broadcasting notifications
- Connection tracking

### SyncProtocol
Unified communication layer:
- Consistent message handling
- Error management
- Timeout handling

## Features

### Automatic Synchronization
- Real-time file change detection
- Cross-device synchronization
- Conflict-free updates

### Session Management
- Multiple devices per user (max 2)
- Automatic reconnection
- Clean session cleanup

### Robust File Handling
- Chunked file transfer
- Integrity verification
- Atomic operations

## Error Handling

The system uses a type-safe `Result` class for error management:

```cpp
enum class SyncError {
    SUCCESS,
    CONNECTION_LOST,
    FILE_NOT_FOUND,
    PERMISSION_DENIED,
    PROTOCOL_ERROR,
    TIMEOUT,
    SESSION_LIMIT_REACHED
};
```

## Dependencies

- C++17 compatible compiler
- POSIX threads
- Isocline library (included)

## Compatibility

- macOS (tested)
- Linux (compatible)
- Modern C++ compilers (GCC 7+, Clang 5+)

## Legacy Code Removed

The following complex legacy components were removed:
- `client/src/sync.cpp` (1449 lines) → Replaced by `SyncClient` class
- `server/src/connection_handler.cpp` (714 lines) → Simplified server architecture
- Complex global state management → Encapsulated in classes
- Mixed threading models → Clean thread separation
- Inconsistent error handling → Unified `Result` system

This refactoring maintains all original functionality while significantly improving code quality, maintainability, and reliability.
