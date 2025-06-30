# Master Makefile for File Synchronization System

# Compiler and flags
CXX = g++
CC = gcc
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -I./common/headers -I./include
CFLAGS = -std=c11
LDFLAGS = -lpthread

# Directories
CLIENT_DIR = client
SERVER_DIR = server
FRONTEND_DIR = frontend
COMMON_DIR = common
CONFIG_DIR = config

# Output
CLIENT_TARGET = $(CLIENT_DIR)/client
SERVER_TARGET = $(SERVER_DIR)/server
FRONTEND_TARGET = $(FRONTEND_DIR)/frontend
PRIMARY_TARGET = $(SERVER_DIR)/primary_server
BACKUP_TARGET = $(SERVER_DIR)/backup_server

# Isocline library
ISOCLINE_SRC = src/isocline.c
ISOCLINE_OBJ = src/isocline.o

# Common sources - now includes replication protocol and cluster config
COMMON_SOURCES = $(COMMON_DIR)/src/socket_utils.cpp $(COMMON_DIR)/src/sync_protocol.cpp \
                 $(COMMON_DIR)/src/replication_protocol.cpp $(COMMON_DIR)/src/cluster_config.cpp
COMMON_OBJECTS = $(COMMON_SOURCES:.cpp=.o)

# Client sources
CLIENT_SOURCES = $(CLIENT_DIR)/src/client.cpp $(CLIENT_DIR)/src/sync_client.cpp
CLIENT_OBJECTS = $(CLIENT_SOURCES:.cpp=.o)

# Server sources - now includes cluster manager and replication components
SERVER_SOURCES = $(SERVER_DIR)/src/server.cpp $(SERVER_DIR)/src/sync_server.cpp \
                 $(SERVER_DIR)/src/file_manager.cpp $(SERVER_DIR)/src/client_manager.cpp \
                 $(SERVER_DIR)/src/cluster_manager.cpp $(SERVER_DIR)/src/operation_log.cpp \
                 $(SERVER_DIR)/src/election_manager.cpp
SERVER_OBJECTS = $(SERVER_SOURCES:.cpp=.o)

# Primary server sources - extends server with replication
PRIMARY_SOURCES = $(SERVER_DIR)/src/primary_main.cpp $(SERVER_DIR)/src/primary_server.cpp $(SERVER_DIR)/src/sync_server.cpp \
                  $(SERVER_DIR)/src/file_manager.cpp $(SERVER_DIR)/src/client_manager.cpp \
                  $(SERVER_DIR)/src/cluster_manager.cpp $(SERVER_DIR)/src/operation_log.cpp \
                  $(SERVER_DIR)/src/election_manager.cpp
PRIMARY_OBJECTS = $(PRIMARY_SOURCES:.cpp=.o)

# Backup server sources
BACKUP_SOURCES = $(SERVER_DIR)/src/backup_main.cpp $(SERVER_DIR)/src/backup_server.cpp $(SERVER_DIR)/src/sync_server.cpp \
                 $(SERVER_DIR)/src/file_manager.cpp $(SERVER_DIR)/src/client_manager.cpp \
                 $(SERVER_DIR)/src/cluster_manager.cpp $(SERVER_DIR)/src/operation_log.cpp \
                 $(SERVER_DIR)/src/election_manager.cpp
BACKUP_OBJECTS = $(BACKUP_SOURCES:.cpp=.o)

# Frontend sources
FRONTEND_SOURCES = $(FRONTEND_DIR)/src/frontend.cpp $(FRONTEND_DIR)/src/frontend_server.cpp \
                   $(FRONTEND_DIR)/src/request_router.cpp
FRONTEND_OBJECTS = $(FRONTEND_SOURCES:.cpp=.o)

.PHONY: all clean client server frontend cluster primary backup replication test-replication

all: client server frontend replication

client: $(CLIENT_TARGET)

server: $(SERVER_TARGET)

frontend: $(FRONTEND_TARGET)

cluster: server frontend

# New replication targets
primary: $(PRIMARY_TARGET)

backup: $(BACKUP_TARGET)

replication: primary backup

$(CLIENT_TARGET): $(CLIENT_OBJECTS) $(COMMON_OBJECTS) $(ISOCLINE_OBJ)
	$(CXX) $(CXXFLAGS) -I$(CLIENT_DIR)/headers $^ -o $@ $(LDFLAGS)

$(SERVER_TARGET): $(SERVER_OBJECTS) $(COMMON_OBJECTS)
	$(CXX) $(CXXFLAGS) -I$(SERVER_DIR)/headers $^ -o $@ $(LDFLAGS)

$(FRONTEND_TARGET): $(FRONTEND_OBJECTS) $(COMMON_OBJECTS)
	$(CXX) $(CXXFLAGS) -I$(FRONTEND_DIR)/headers $^ -o $@ $(LDFLAGS)

$(PRIMARY_TARGET): $(PRIMARY_OBJECTS) $(COMMON_OBJECTS)
	$(CXX) $(CXXFLAGS) -I$(SERVER_DIR)/headers $^ -o $@ $(LDFLAGS)

$(BACKUP_TARGET): $(BACKUP_OBJECTS) $(COMMON_OBJECTS)
	$(CXX) $(CXXFLAGS) -I$(SERVER_DIR)/headers $^ -o $@ $(LDFLAGS)

# Isocline library
$(ISOCLINE_OBJ): $(ISOCLINE_SRC)
	$(CC) $(CFLAGS) -c $< -o $@

# Pattern rules for object files
$(CLIENT_DIR)/%.o: $(CLIENT_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -I$(CLIENT_DIR)/headers -c $< -o $@

$(SERVER_DIR)/%.o: $(SERVER_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -I$(SERVER_DIR)/headers -c $< -o $@

$(FRONTEND_DIR)/%.o: $(FRONTEND_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -I$(FRONTEND_DIR)/headers -c $< -o $@

$(COMMON_DIR)/%.o: $(COMMON_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Configuration testing
test-config: $(COMMON_OBJECTS)
	@echo "Testing cluster configuration..."
	$(CXX) $(CXXFLAGS) -DTEST_CONFIG -o test_config \
		$(COMMON_DIR)/src/cluster_config.cpp $(COMMON_DIR)/src/replication_protocol.cpp \
		$(COMMON_DIR)/src/sync_protocol.cpp $(COMMON_DIR)/src/socket_utils.cpp \
		$(LDFLAGS) && ./test_config $(CONFIG_DIR)/cluster.conf && rm -f test_config

# Replication testing
test-replication: replication
	@echo "Testing replication system..."
	@echo "Starting primary server on port 8001..."
	@./$(PRIMARY_TARGET) -p 8001 -c $(CONFIG_DIR)/cluster.conf &
	@sleep 2
	@echo "Starting backup server on port 8002..."
	@./$(BACKUP_TARGET) -p 8002 -c $(CONFIG_DIR)/cluster.conf &
	@sleep 2
	@echo "Replication test setup complete. Use Ctrl+C to stop servers."

clean:
	rm -f $(CLIENT_OBJECTS) $(SERVER_OBJECTS) $(PRIMARY_OBJECTS) $(BACKUP_OBJECTS) \
		  $(FRONTEND_OBJECTS) $(COMMON_OBJECTS) $(ISOCLINE_OBJ)
	rm -f $(CLIENT_TARGET) $(SERVER_TARGET) $(FRONTEND_TARGET) \
		  $(PRIMARY_TARGET) $(BACKUP_TARGET)
	rm -rf sync_dir_* files/
	rm -f test_config

install: all replication
	@echo "File Synchronization System built successfully!"
	@echo "Client: $(CLIENT_TARGET)"
	@echo "Server: $(SERVER_TARGET)"
	@echo "Frontend: $(FRONTEND_TARGET)"
	@echo "Primary Server: $(PRIMARY_TARGET)"
	@echo "Backup Server: $(BACKUP_TARGET)"
	@echo "Config: $(CONFIG_DIR)/cluster.conf"

help:
	@echo "Available targets:"
	@echo "  all           - Build client, server, and frontend"
	@echo "  client        - Build only client"
	@echo "  server        - Build only server"
	@echo "  frontend      - Build only frontend"
	@echo "  primary       - Build primary server with replication"
	@echo "  backup        - Build backup server"
	@echo "  replication   - Build both primary and backup servers"
	@echo "  cluster       - Build server and frontend (cluster components)"
	@echo "  test-config   - Test cluster configuration loading"
	@echo "  test-replication - Test replication system"
	@echo "  clean         - Remove all build files and sync directories"
	@echo "  install       - Build and show installation info"
	@echo "  help          - Show this help message"
