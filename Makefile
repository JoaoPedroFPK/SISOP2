.PHONY: all server client clean

SERVER_SRC = $(wildcard server/src/*.cpp) $(wildcard common/src/*.cpp)
CLIENT_SRC = $(wildcard client/src/*.cpp) $(wildcard common/src/*.cpp) src/isocline.c
INCLUDES   = -I include/ -Iclient/headers -Iserver/headers -Icommon/headers

CXXFLAGS = -std=c++17 -pthread

all: server client

server:
	g++ $(CXXFLAGS) -o server/server $(SERVER_SRC) $(INCLUDES)

client:
	g++ $(CXXFLAGS) -o client/client $(CLIENT_SRC) $(INCLUDES)

clean:
	rm -f server/server client/client
