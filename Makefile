.PHONY: all server client clean

CPP 	 =  clang++
CPPFLAGS = -std=c++17
CPPFLAGS += -Icommon/headers -Iclient/headers -Iserver/headers -I.
CPPFLAGS += -Iclient/tests/
CPPFLAGS += -pthread
CPPFLAGS += -Wall -Wextra

all: server client


server:
	$(CPP) $(CPPFLAGS) -o server/server server/src/*.cpp common/src/*.cpp

client:
	$(CPP) $(CPPFLAGS) -o client/client client/src/*.cpp common/src/*.cpp

clean:
	rm -rf server/bin client/bin
