#pragma once

struct server_params_t
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

__attribute__((noreturn)) void server_process(struct server_params_t* params);
