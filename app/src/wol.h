/* Wake-on-LAN: the magic packet that wakes a sleeping PC. */
#pragma once
#include <stdbool.h>

/* Sends the magic packet for mac ("aa:bb:cc:dd:ee:ff") to the network's broadcast addresses and to host, on the ports
 * Moonlight uses (9, 47009 and the GameStream ones). False if mac isn't a MAC address or nothing could be sent. */
bool wol_send(const char *host, const char *mac);
