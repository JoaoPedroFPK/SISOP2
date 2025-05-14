#!/bin/bash

# Global variable to track test success/failure
TEST_SUCCESS=true

# Function to print logs from all containers
print_all_logs() {
  echo -e "\n${RED}=== TEST FAILED - PRINTING ALL LOGS ===${NC}"
  
  echo -e "\n${RED}=== SERVER LOGS ===${NC}"
  docker logs server
  
  echo -e "\n${RED}=== CLIENT1 LOGS ===${NC}"
  docker logs client1
  
  echo -e "\n${RED}=== CLIENT2 LOGS ===${NC}"
  docker logs client2
  
  echo -e "\n${RED}=== END OF LOGS ===${NC}"
}

# Function to mark test as failed
mark_as_failed() {
  TEST_SUCCESS=false
}

# Test script to verify sync_dir functionality across multiple devices
# This test will:
# 1. Build the latest application
# 2. Start server and two clients with docker-compose
# 3. Create a file in client1's sync_dir
# 4. Verify the file appears in client2 and on the server

set -e  # Exit immediately if a command exits with a non-zero status

# Define colors for output
GREEN="\033[0;32m"
RED="\033[0;31m"
NC="\033[0m" # No Color

# Define test parameters
TEST_USER="user-test-case-1"
SERVER_PORT="5001"
SERVER_HOST="server"
TEST_FILENAME="test_sync_file.txt"
TEST_CONTENT="This is a test file for synchronization testing $(date)"

echo "========== Starting Sync Test =========="

# Step 1: Build the latest version of the application
echo "Building the latest version of the application..."
cd "$(dirname "$0")/.." # Move to project root
make clean && make

# Step 2: Stop any running containers and start fresh
echo "Starting docker containers..."
docker-compose down -v # Clean previous volumes
docker-compose up -d

# Give server time to start up and complete healthcheck
echo "Waiting for server to be ready..."
sleep 10

# Step 3: Verify server container is running
if ! docker ps | grep -q "server"; then
    echo -e "${RED}[FAILED]${NC} Server container is not running!"
    mark_as_failed
    print_all_logs
    exit 1
fi

# Step 4: Start server application on specified port
echo "Starting server on port ${SERVER_PORT}..."
docker exec -i server sh -c "./server/server ${SERVER_PORT}" &
SERVER_PID=$!

# Give server more time to initialize
sleep 5

# Step 5: Start clients with the same username
echo "Starting client1..."
docker exec -i client1 sh -c "./client/client ${TEST_USER} ${SERVER_HOST} ${SERVER_PORT}" &
CLIENT1_PID=$!

echo "Starting client2..."
docker exec -i client2 sh -c "./client/client ${TEST_USER} ${SERVER_HOST} ${SERVER_PORT}" &
CLIENT2_PID=$!

# Allow time for clients to connect and initialize sync_dir
sleep 10

# Step 6: Wait to ensure sync_dir is fully initialized
echo "Waiting for sync directories to be fully initialized..."
sleep 5

# Step 7: Create a test file in client1's sync_dir
echo "Creating test file in client1's sync_dir..."
TEST_FILE_PATH="/app/client/sync_dir_${TEST_USER}/${TEST_FILENAME}"
docker exec -i client1 sh -c "echo '${TEST_CONTENT}' > ${TEST_FILE_PATH} && sync" # sync ensures file is written to disk

# Step 8: Wait for synchronization to occur
echo "Waiting for synchronization to occur..."
sleep 15 # Give more time for synchronization to complete

# Step 9: Verify file exists in client2's sync_dir
echo "Verifying file exists in client2's sync_dir..."
if docker exec -i client2 sh -c "test -f /app/client/sync_dir_${TEST_USER}/${TEST_FILENAME}"; then
    echo -e "${GREEN}[PASSED]${NC} File found in client2's sync_dir"
    
    # Verify file content in client2
    CLIENT2_CONTENT=$(docker exec -i client2 sh -c "cat /app/client/sync_dir_${TEST_USER}/${TEST_FILENAME}")
    if [ "$CLIENT2_CONTENT" = "$TEST_CONTENT" ]; then
        echo -e "${GREEN}[PASSED]${NC} File content in client2 matches original"
    else
        echo -e "${RED}[FAILED]${NC} File content in client2 doesn't match original"
        echo "Expected: $TEST_CONTENT"
        echo "Actual: $CLIENT2_CONTENT"
        mark_as_failed
    fi
else
    echo -e "${RED}[FAILED]${NC} File not found in client2's sync_dir"
    mark_as_failed
fi

# Step 10: Verify file exists on server
echo "Verifying file exists on server..."
if docker exec -i server sh -c "test -f /app/server/files/sync_dir_${TEST_USER}/${TEST_FILENAME}"; then
    echo -e "${GREEN}[PASSED]${NC} File found on server"
    
    # Verify file content on server
    SERVER_CONTENT=$(docker exec -i server sh -c "cat /app/server/files/sync_dir_${TEST_USER}/${TEST_FILENAME}")
    if [ "$SERVER_CONTENT" = "$TEST_CONTENT" ]; then
        echo -e "${GREEN}[PASSED]${NC} File content on server matches original"
    else
        echo -e "${RED}[FAILED]${NC} File content on server doesn't match original"
        echo "Expected: $TEST_CONTENT"
        echo "Actual: $SERVER_CONTENT"
        mark_as_failed
    fi
else
    echo -e "${RED}[FAILED]${NC} File not found on server"
    mark_as_failed
fi

# Step 11: List files in both sync_dirs and server for debugging
echo -e "\nListing files in client1's sync_dir:"
docker exec -i client1 sh -c "ls -la /app/client/sync_dir_${TEST_USER}/"

echo -e "\nListing files in client2's sync_dir:"
docker exec -i client2 sh -c "ls -la /app/client/sync_dir_${TEST_USER}/"

echo -e "\nListing files on server:"
docker exec -i server sh -c "ls -la /app/server/files/sync_dir_${TEST_USER}/"

# Step 12: Clean up processes (kill background processes)
echo "Cleaning up background processes..."
# The commands below will gracefully terminate only if they're still running
kill $SERVER_PID 2>/dev/null || true
kill $CLIENT1_PID 2>/dev/null || true
kill $CLIENT2_PID 2>/dev/null || true

# Optional: Clean up containers
# Uncomment the following line to stop containers after test
# docker-compose down

echo "========== Sync Test Complete ==========="

# Print all logs if any test has failed
if [ "$TEST_SUCCESS" = false ]; then
    print_all_logs
    exit 1
fi

# Return success if all tests passed
exit 0
