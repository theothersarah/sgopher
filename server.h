#pragma once

struct server_params
{
	// Network
	unsigned short port;
	
	// Client management
	int maxClients;
	unsigned int timeout;
	
	// Paths and files
	const char* directory;
	const char* indexFile;
};

int doserver(struct server_params* params);
