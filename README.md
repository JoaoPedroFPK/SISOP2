# File Synchronization System

This project implements a client-server file synchronization system in C++. It allows clients to connect to a server and synchronize files between different machines.

## Project Structure

- `client/`: Client-side application code
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

## Implementation Details

- The system uses TCP sockets for reliable communication
- Custom packet format for data transmission
- Threaded connection handling on the server side
- File synchronization capabilities
