/* Finds PCs running Sunshine (or Apollo, or GameStream) on the console's network, in the background: each address of
 * its /24 (XAM gives no netmask) is tried on GameStream's HTTP port, and those that answer /serverinfo are listed by
 * name. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

enum { DISCOVER_MAX = 16 };

/* Starts a search (a fresh list); nothing if one is running. */
void discover_start(void);
bool discover_running(void);
/* Addresses tried so far, of total. */
void discover_progress(int *tried, int *total);
int discover_count(void);
/* PC i's address ("192.168.1.35") and name ("GAMING-PC"; the address if it gave none). */
void discover_get(int i, char *address, size_t address_size, char *name, size_t name_size);
/* Stops a search (before a stream). Doesn't wait. */
void discover_stop(void);
