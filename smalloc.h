#pragma once

// size_t
#include <stddef.h>

// Allocate shared memory
void* smalloc(size_t size);

// Allocate shared memory sized to fit an array of objects
void* scalloc(size_t nmemb, size_t size);

// Free shared memory
void sfree(void* ptr);
