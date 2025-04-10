#!/bin/bash

# Colors for better output
GREEN='\033[1;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
SERVER_DIR="../server"
CLIENT_DIR="../client"
TEST_USERNAME="testuser"
SERVER_PORT=8000
TEST_FILE="test_file.txt"
SERVER_SYNC_DIR="sync_dir_${TEST_USERNAME}"

# Print header
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}   Dropbox-like System - Upload Test    ${NC}"
echo -e "${BLUE}========================================${NC}"

# Step 1: Clean up previous test state
echo -e "\n${BLUE}[1/6]${NC} Cleaning up previous test state..."
rm -f ${TEST_FILE}
rm -rf ${SERVER_DIR}/${SERVER_SYNC_DIR}
mkdir -p ${CLIENT_DIR}

# Ensure all necessary server directories exist
mkdir -p ${SERVER_DIR}/files
mkdir -p ${SERVER_DIR}/files/sync_dir_${TEST_USERNAME}
chmod -R 755 ${SERVER_DIR}/files

echo -e "${GREEN}✓ Cleanup completed${NC}"

# Step 2: Build the project
echo -e "\n${BLUE}[2/6]${NC} Building the project..."
cd ..
make clean && make
if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Build successful${NC}"
cd tests

# Step 3: Create test file
echo -e "\n${BLUE}[3/6]${NC} Creating test file..."
echo "This is a test file for the upload functionality" > ${TEST_FILE}
if [ ! -f ${TEST_FILE} ]; then
    echo -e "${RED}✗ Failed to create test file${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Test file created${NC}"

# Step 4: Start the server
echo -e "\n${BLUE}[4/6]${NC} Starting the server..."

# Create the server/files directory in the project root
mkdir -p ../server/files

# Run the server from the project root
cd ..
./server/server ${SERVER_PORT} > tests/server_log.txt 2>&1 &
SERVER_PID=$!
cd tests
sleep 2 # Give the server time to start

# Check if server is running
if ! ps -p ${SERVER_PID} > /dev/null; then
    echo -e "${RED}✗ Server failed to start${NC}"
    cat server_log.txt
    exit 1
fi
echo -e "${GREEN}✓ Server started with PID ${SERVER_PID}${NC}"

# Step 5: Run the client and perform upload
echo -e "\n${BLUE}[5/6]${NC} Starting the client and executing upload command..."

# Create expect script to automate client interaction
cat > client_commands.exp << EOF
#!/usr/bin/expect -f
set timeout 10
spawn ../client/client ${TEST_USERNAME} localhost ${SERVER_PORT}
expect ">"
send "upload ${TEST_FILE}\r"
expect "enviado com sucesso"
send "exit\r"
expect eof
EOF

chmod +x client_commands.exp
./client_commands.exp > client_log.txt 2>&1

if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Client upload failed${NC}"
    cat client_log.txt
    kill ${SERVER_PID}
    exit 1
fi
echo -e "${GREEN}✓ Client upload command executed${NC}"

# Step 6: Verify file was uploaded to server
echo -e "\n${BLUE}[6/6]${NC} Verifying file was uploaded to server..."
sleep 2 # Give time for synchronization

# The server saves files in server/files/sync_dir_username
# Check if file exists in server's sync directory
if [ -f "../server/files/sync_dir_${TEST_USERNAME}/${TEST_FILE}" ]; then
    echo -e "${GREEN}✓ Test file found in server's directory${NC}"
    # Compare file contents
    diff ${TEST_FILE} "../server/files/sync_dir_${TEST_USERNAME}/${TEST_FILE}" > /dev/null
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓ File content matches${NC}"
        TEST_RESULT="PASSED"
    else
        echo -e "${RED}✗ File content does not match${NC}"
        TEST_RESULT="FAILED - Content mismatch"
    fi
else
    echo -e "${RED}✗ Test file not found in server's directory${NC}"
    ls -la ../server/files/ || echo "Cannot list server files directory"
    TEST_RESULT="FAILED - File not found"
fi

# Clean up
echo -e "\n${BLUE}Cleaning up...${NC}"
kill ${SERVER_PID}
rm -f ${TEST_FILE} client_commands.exp client_log.txt server_log.txt

# Print test summary
echo -e "\n${BLUE}========================================${NC}"
echo -e "${BLUE}             Test Summary              ${NC}"
echo -e "${BLUE}========================================${NC}"
if [[ ${TEST_RESULT} == PASSED* ]]; then
    echo -e "${GREEN}Upload Test: PASSED${NC}"
else
    echo -e "${RED}Upload Test: ${TEST_RESULT}${NC}"
fi
echo -e "${BLUE}========================================${NC}"

# Exit with appropriate status
if [[ ${TEST_RESULT} == PASSED* ]]; then
    exit 0
else
    exit 1
fi 