/* Workarounds for the console's network stack (XNet in XAM) under Moonlight's load. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Restarts XNet (WSACleanup, XNetCleanup, XNetStartup, WSAStartup) with the title's limits. Only with no socket
 * open. Used before each connection: after a heavy stream XNet has been seen to stop reaching the host until the
 * title restarted, and a restart of XNet is what a title restart does. */
void netfix_restart_network(void);

/* Received-datagram sizes per socket since the last reset, into the log. */
void netfix_log_datagrams(void);
void netfix_reset_datagrams(void);

/* Wi-Fi keepalive: sends small UDP packets to host:9 (discard) at pps a second until stopped, so the console's
 * Wi-Fi adapter never dozes between the host's bursts. 0 stops it. */
void netfix_keepalive(const char *host, int pps);

/* "The 360 way": no thread ever blocks inside the network stack. poll() and timed UDP receives become
 * non-blocking calls with 1 ms sleeps between them, in the title's own code. Set before any socket is made. */
void netfix_set_nonblocking_waits(bool on);
/* "fd:count ..." for every socket that received datagrams, for a timeline in the log. */
void netfix_snapshot(char *out, size_t size);
