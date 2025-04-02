all: server client

server:
	g++ -o server/server server/*.cpp commons/*.cpp -pthread

client:
	g++ -o client/client client/*.cpp commons/*.cpp -pthread

clean:
	rm -f server/server client/client
