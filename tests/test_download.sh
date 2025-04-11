#!/bin/bash

# Colors for better output
GREEN='\033[1;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
YEL='\033[0;33m'
NC='\033[0m' # No Color

# Configuration
SERVER_DIR="../server"
CLIENT_DIR="../client"
TEST_USERNAME="testuser"
SERVER_PORT=8000
TEST_FILE="test_file.txt"
SERVER_SYNC_DIR="sync_dir_${TEST_USERNAME}"

# Print header
echo -e "${BLUE}==========================================${NC}"
echo -e "${BLUE}  Dropbox-like System - Download Test     ${NC}"
echo -e "${BLUE}==========================================${NC}"

# Step 1: Clean up previous test state
echo -e "\n${BLUE}[1/5]${NC} Cleaning up previous test state..."
rm -f "./${TEST_FILE}"

# Ensure necessary server directories exist
mkdir -p ${SERVER_DIR}/files/sync_dir_${TEST_USERNAME}
chmod -R 755 ${SERVER_DIR}/files

# Verify test file exists on server
if [ ! -f "${SERVER_DIR}/files/sync_dir_${TEST_USERNAME}/${TEST_FILE}" ]; then
    echo -e "${RED}✗ Required test file does not exist on the server: ${SERVER_DIR}/files/sync_dir_${TEST_USERNAME}/${TEST_FILE}${NC}"
    echo -e "${YEL} SUGGESTTION: Run the upload test (test_upload.sh) first to create the test file (test_file.txt).${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Test file exists on server${NC}"

# Step 2: Build the project
echo -e "\n${BLUE}[2/5]${NC} Building the project..."
cd ..
make clean && make
if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Build successful${NC}"
cd tests

# Step 3: Start the server
echo -e "\n${BLUE}[3/5]${NC} Starting the server..."
cd ..
./server/server ${SERVER_PORT} > tests/server_log.txt 2>&1 &
SERVER_PID=$!
cd tests
sleep 2

if ! ps -p ${SERVER_PID} > /dev/null; then
    echo -e "${RED}✗ Server failed to start${NC}"
    cat server_log.txt
    exit 1
fi
echo -e "${GREEN}✓ Server started with PID ${SERVER_PID}${NC}"

# Step 4: Run the client and perform download
echo -e "\n${BLUE}[4/5]${NC} Starting the client and executing download command..."

cat > client_commands.exp << EOF
#!/usr/bin/expect -f
set timeout 10
spawn ../client/client ${TEST_USERNAME} localhost ${SERVER_PORT}
expect ">"
send "download ${TEST_FILE}\r"
expect "recebido com sucesso"
send "exit\r"
expect eof
EOF

chmod +x client_commands.exp
./client_commands.exp > client_log.txt 2>&1

if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Client download failed${NC}"
    cat client_log.txt
    kill ${SERVER_PID}
    exit 1
fi
echo -e "${GREEN}✓ Client download command executed${NC}"

# Step 5: Verify downloaded file
echo -e "\n${BLUE}[5/5]${NC} Verifying file was downloaded to current directory..."
sleep 2

if [ -f "./${TEST_FILE}" ]; then
    echo -e "${GREEN}✓ File found in current directory${NC}"
    diff "./${TEST_FILE}" "${SERVER_DIR}/files/sync_dir_${TEST_USERNAME}/${TEST_FILE}" > /dev/null
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓ File content matches${NC}"
        TEST_RESULT="PASSED"
    else
        echo -e "${RED}✗ File content does not match${NC}"
        TEST_RESULT="FAILED - Content mismatch"
    fi
else
    echo -e "${RED}✗ File not found in current directory${NC}"
    TEST_RESULT="FAILED - File not downloaded"
fi

# Clean up
echo -e "\n${BLUE}Cleaning up...${NC}"
kill ${SERVER_PID}
rm -f client_commands.exp client_log.txt server_log.txt "./${TEST_FILE}"

# Print test summary
echo -e "\n${BLUE}========================================${NC}"
echo -e "${BLUE}             Test Summary              ${NC}"
echo -e "${BLUE}========================================${NC}"
if [[ ${TEST_RESULT} == PASSED* ]]; then
    echo -e "${GREEN}Download Test: PASSED${NC}"
else
    echo -e "${RED}Download Test: ${TEST_RESULT}${NC}"
fi
echo -e "${BLUE}========================================${NC}"

# Exit with appropriate status
if [[ ${TEST_RESULT} == PASSED* ]]; then
    exit 0
else
    exit 1
fi
