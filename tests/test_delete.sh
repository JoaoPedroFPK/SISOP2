#!/bin/bash

# Colors
GREEN='\033[1;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

# Configuration
SERVER_DIR="../server"
CLIENT_DIR="../client"
TEST_USERNAME="testuser"
SERVER_PORT=8000
TEST_FILE="test_file.txt"
SYNC_DIR="sync_dir_${TEST_USERNAME}"
SERVER_SYNC_PATH="${SERVER_DIR}/files/${SYNC_DIR}"
CLIENT_SYNC_PATH="${CLIENT_DIR}/files/${SYNC_DIR}"
TEST_SYNC_PATH="${SYNC_DIR}/files/${SYNC_DIR}"

# Header
echo -e "${BLUE}==========================================${NC}"
echo -e "${BLUE}    Dropbox-like System - Delete Test     ${NC}"
echo -e "${BLUE}==========================================${NC}"

# Step 1: Clean up
echo -e "\n${BLUE}[1/6]${NC} Cleaning up previous test state..."
rm -f client_commands.exp client_log.txt server_log.txt
rm -rf "${SERVER_SYNC_PATH}" "${CLIENT_SYNC_PATH}"
mkdir -p "${SERVER_SYNC_PATH}" "${CLIENT_SYNC_PATH}"

# Create the test file in server and client sync directories
echo "This is a test file for deletion." > "${SERVER_SYNC_PATH}/${TEST_FILE}"
cp "${SERVER_SYNC_PATH}/${TEST_FILE}" "${CLIENT_SYNC_PATH}/${TEST_FILE}"

echo -e "${GREEN}✓ Environment prepared with synced test file${NC}"

# Step 2: Build project
echo -e "\n${BLUE}[2/6]${NC} Building the project..."
cd ..
make clean && make
if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Build successful${NC}"
cd tests

# Step 3: Start server
echo -e "\n${BLUE}[3/6]${NC} Starting the server..."
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

# Step 4: Client sends 'delete' command
echo -e "\n${BLUE}[4/6]${NC} Executing delete command from client..."

cat > client_commands.exp << EOF
#!/usr/bin/expect -f
set timeout 10
spawn ../client/client ${TEST_USERNAME} localhost ${SERVER_PORT}
expect ">"
send "delete ${TEST_FILE}\r"
expect "deletado com sucesso"
send "exit\r"
expect eof
EOF

chmod +x client_commands.exp
./client_commands.exp > client_log.txt 2>&1

if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Client delete command failed${NC}"
    cat client_log.txt
    kill ${SERVER_PID}
    exit 1
fi
echo -e "${GREEN}✓ Client executed delete command${NC}"

# Step 5: Verify deletion from client and server sync dirs
echo -e "\n${BLUE}[5/6]${NC} Verifying deletion from sync directories..."
sleep 2

MISSING=""
for path in "${TEST_SYNC_PATH}" "${SERVER_SYNC_PATH}"; do
    if [ -f "${path}/${TEST_FILE}" ]; then
        echo -e "${RED}✗ File still exists in: ${path}${NC}"
        MISSING="false"
    else
        echo -e "${GREEN}✓ File deleted from: ${path}${NC}"
    fi
done

# Step 6: Attempt to delete non-existent file
echo -e "\n${BLUE}[6/6]${NC} Attempting to delete non-existent file..."

cat > client_commands.exp << EOF
#!/usr/bin/expect -f
set timeout 10
spawn ../client/client ${TEST_USERNAME} localhost ${SERVER_PORT}
expect ">"
send "delete arquivo_inexistente.txt\r"
expect "arquivo não encontrado"
send "exit\r"
expect eof
EOF

./client_commands.exp >> client_log.txt 2>&1

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Handled deletion of non-existent file gracefully${NC}"
else
    echo -e "${RED}✗ Failed handling deletion of non-existent file${NC}"
    MISSING="false"
fi

# Cleanup
echo -e "\n${BLUE}Cleaning up...${NC}"
kill ${SERVER_PID}
rm -f client_commands.exp client_log.txt server_log.txt

# Summary
echo -e "\n${BLUE}========================================${NC}"
echo -e "${BLUE}             Test Summary              ${NC}"
echo -e "${BLUE}========================================${NC}"
if [ -z "$MISSING" ]; then
    echo -e "${GREEN}Delete Test: PASSED${NC}"
    exit 0
else
    echo -e "${RED}Delete Test: FAILED${NC}"
    exit 1
fi
