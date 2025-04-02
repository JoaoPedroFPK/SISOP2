.PHONY: all server client clean

all: server client

server:
	g++ -std=c++17 -o server/server server/*.cpp common/*.cpp -pthread

client:
	g++ -std=c++17 -o client/client client/*.cpp common/*.cpp -pthread

clean:
	rm -f server/server client/client
