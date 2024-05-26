#pragma once

struct server_params
{
	// Network
	const char* hostname;
	unsigned short port;
	
	// Client management
	unsigned int maxClients;
	unsigned int timeout;
	
	// Paths and files
	const char* directory;
	const char* indexfile;
};

int doserver(struct server_params* params);
