/* Text for the PC: typed on the console's on-screen keyboard during a stream, then sent as if typed there. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Opens the keyboard for the pad's user; false if it didn't open. */
bool typetext_open(uint32_t user);
/* Call once a frame: true while the keyboard is open. Sends the text when it closes with some. */
bool typetext_poll(void);
