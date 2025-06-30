.PHONY: all server client clean

SERVER_SRC = $(wildcard server/src/*.cpp) $(wildcard common/src/*.cpp)
CLIENT_CPP_SRC = $(wildcard client/src/*.cpp) $(wildcard common/src/*.cpp)
CLIENT_C_SRC  = src/isocline.c
CLIENT_OBJ    = $(CLIENT_CPP_SRC:.cpp=.o) src/isocline.o
INCLUDES   = -I include/ -Iclient/headers -Iserver/headers -Icommon/headers

CXXFLAGS = -std=c++17 -pthread -Wall -Wextra -Wpedantic -O0
CCFLAGS  = -std=c11

all: server client
	mkdir -p servidor cliente1 cliente2 backup1 backup2
	cp server/server servidor/
	cp client/client cliente1/
	cp client/client cliente2/
	cp server/server backup1/
	cp server/server backup2/

server:
	g++ $(CXXFLAGS) -o server/server $(SERVER_SRC) $(INCLUDES)

client: src/isocline.o
	g++ $(CXXFLAGS) -o client/client $(CLIENT_CPP_SRC) src/isocline.o $(INCLUDES)

src/isocline.o: $(CLIENT_C_SRC)
	gcc $(CCFLAGS) -c $< -o $@

clean:
	rm src/isocline.o
	rm -f server/server client/client
	rm -rf servidor/*
	rm -rf cliente1/*
	rm -rf cliente2/*
	rm -rf backup1/*
	rm -rf backup2/*
