# File Synchronization System

This project implements a Dropbox-like file synchronization system in C++. It allows users to synchronize files across multiple devices through a central server, with automatic synchronization of changes.

## Project Overview

This system allows users to:
- Maintain synchronized files across multiple devices
- Upload and download files to/from the server
- Delete files from the synchronized directory
- List files on both client and server
- Connect with up to two devices simultaneously per user

## Project Structure

- `client/`: Client-side application code
  - `client_interface.cpp`: Command processing and user interface
  - `sync.h/cpp`: Synchronization functionality
- `server/`: Server-side application code
- `common/`: Shared code between client and server
- `tests/`: Test files for the application
- `Dockerfile` and `docker-compose.yml`: Container configuration for easy deployment

## Building the Project

Use the provided Makefile to build both the client and server:

```bash
# Build both client and server
make

# Build only the server
make server

# Build only the client
make client

# Clean build files
make clean
```

## Running with Docker

The project can be run using Docker containers:

```bash
# Start the server and client containers
docker-compose up -d

# Connect to the client container to run commands
docker exec -it client /bin/bash
```

## Usage

### Server

```bash
./server/server <port>
```

### Client

```bash
./client/client <username> <server_ip_address> <port>
```

### Available Commands

Once connected, the following commands are available:

- `upload <path/filename.ext>` - Upload a file to the server
- `download <filename.ext>` - Download a file from server to local directory
- `delete <filename.ext>` - Remove a file from the synchronization directory
- `list_server` - List files stored on the server
- `list_client` - List files in the local sync directory
- `get_sync_dir` - Initialize the synchronization directory
- `exit` - Close connection with the server
- `help` - Display available commands

## Implementation Details

- **Communication Protocol**: The system uses TCP sockets for reliable communication
- **Packet Structure**: Custom packet format for data transmission
- **Concurrency**: Threaded connection handling on the server side to support multiple clients
- **Synchronization**: Automatic file synchronization between client and server
- **Directory Monitoring**: Watches local sync directory for changes
- **Data Persistence**: Server stores files and restores state between sessions

## Current Progress

So far, we have implemented:

- Basic client interface for command processing
- Command constants for improved code maintainability
- Command parsing and handling
- Client-side UI with help information
- Project structure and organization

Work in progress:
- Server-side implementation for handling multiple users
- File synchronization mechanism
- Directory monitoring
- TCP socket communication implementation
- Testing infrastructure

## Requirements

- Unix/Linux operating system
- C++11 or later compiler
- POSIX-compliant system for socket programming

## Technical Implementation

The system is built using:
- TCP sockets for client-server communication
- Multithreading for concurrent client connections
- Mutexes and semaphores for synchronization
- File I/O operations for reading and writing files
- Custom packet format for data transfer
