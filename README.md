# Dropbox-like Sync Application

This application implements a Dropbox-like service to allow file synchronization between devices. It provides user authentication, file upload/download capabilities, and automatic synchronization across multiple devices for the same user.

## Building the Application

You can build the application in two ways: directly with Make or using Docker containers.

### Building with Make

```bash
# Clean and build the application
make clean && make
```

This will generate the server and client executables in their respective directories.

### Building with Docker

```bash
# Build the Docker containers
docker-compose build
```

## Running the Server

### Running the Server Directly

```bash
# Start the server on port 8000
./server/server 8000
```

You should see the message: `Servidor rodando na porta 8000...`

### Running the Server in Docker

```bash
# Start the server container
docker-compose up server
```

## Running the Client

### Running the Client Directly

```bash
# Connect client with a username to the server
./client/client <username> <server_ip> <port>

# Example:
./client/client testuser 127.0.0.1 8000
```

If running in the same machine, use `127.0.0.1` as the server IP. If you're using Docker, use the container's IP address or hostname.

### Running the Client in Docker

```bash
# Start the client container
docker-compose up client
```

You'll need to modify the docker-compose.yml file to set the correct username, server IP, and port.

## Testing the Upload Command

1. Start the server
2. Start the client
3. Create a test file (if needed): `echo "This is a test file" > test_file.txt`
4. In the client interface, upload the file: `upload test_file.txt`

The client should display debugging information during the upload process and confirm when the file has been successfully uploaded.

You can verify the upload worked by:
- Using the `list_server` command to see files on the server
- Using the `list_client` command to see files in your local sync directory
- Checking the `sync_dir_<username>` directory on your machine
- Checking the `server_files/sync_dir_<username>` directory on the server

## Testing Other Commands

### Download a File

```
download <filename>
```

This will download a copy of the file from the server to your local directory.

### Delete a File

```
delete <filename>
```

This will delete the file from the sync directory and the server.

### List Server Files

```
list_server
```

This shows files stored on the server for your user.

### List Client Files

```
list_client
```

This shows files in your local sync directory.

### Exit

```
exit
```

This closes the connection with the server.

## Automated Testing

You can also use the provided test scripts to automate testing:

```bash
# Run the upload test
./upload_test.sh
```

This script will:
1. Create a test file
2. Start the server
3. Run a client with commands to upload and list files
4. Show the results, including file listings on both server and client
5. Clean up after the test

## Troubleshooting

### Connection Issues
- Make sure the server is running and accessible from the client
- Check that you're using the correct IP address and port

### Upload Problems
- Check that the file exists in the current directory
- Ensure the server has write permissions to its storage directory

### Synchronization Issues
- Verify that the sync_dir exists and is writeable
- Check client logs for any errors during synchronization
