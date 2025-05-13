.PHONY: all server client clean

all: server client

server:
	g++ -std=c++17 -o server/server server/src/*.cpp common/src/*.cpp -pthread

client:
	g++ -std=c++17 -o client/client client/src/*.cpp common/src/*.cpp src/isocline.c include/isocline.h -pthread -I include/

clean:
	rm -f server/server client/client
