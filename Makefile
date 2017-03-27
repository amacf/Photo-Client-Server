all: server client

server: server.cpp
	g++ -o server server.cpp

client: photoClient.cpp
	g++ -o client photoClient.cpp
