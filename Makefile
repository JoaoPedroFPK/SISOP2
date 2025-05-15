.PHONY: all server client clean

SERVER_SRC = $(wildcard server/src/*.cpp) $(wildcard common/src/*.cpp)
CLIENT_CPP_SRC = $(wildcard client/src/*.cpp) $(wildcard common/src/*.cpp)
CLIENT_C_SRC  = src/isocline.c
CLIENT_OBJ    = $(CLIENT_CPP_SRC:.cpp=.o) src/isocline.o
INCLUDES   = -I include/ -Iclient/headers -Iserver/headers -Icommon/headers

CXXFLAGS = -std=c++17 -pthread -Wall -Wextra -Wpedantic -O0 -g3
CCFLAGS  = -std=c11

all: server client

server:
	g++ $(CXXFLAGS) -o server/server $(SERVER_SRC) $(INCLUDES)

client: src/isocline.o
	g++ $(CXXFLAGS) -o client/client $(CLIENT_CPP_SRC) src/isocline.o $(INCLUDES)

src/isocline.o: $(CLIENT_C_SRC)
	gcc $(CCFLAGS) -c $< -o $@

clean:
	rm -f server/server client/client
	rm src/isocline.o
