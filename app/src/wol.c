#include "wol.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool parse_mac(const char *text, uint8_t mac[6])
{
    unsigned v[6];
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6 &&
        sscanf(text, "%2x-%2x-%2x-%2x-%2x-%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    unsigned any = 0;
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)v[i];
        any |= v[i];
    }
    return any != 0;   /* all zeros: a host that didn't say */
}

bool wol_send(const char *host, const char *mac_text)
{
    uint8_t mac[6];
    if (!parse_mac(mac_text, mac))
        return false;
    /* Six 0xFF bytes, then the MAC sixteen times. */
    uint8_t packet[6 + 16 * 6];
    memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; i++)
        memcpy(packet + 6 + i * 6, mac, 6);

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    /* Everywhere it might be heard: everyone, the host's /24 (most home networks), and the host itself. */
    in_addr_t targets[3];
    int count = 0;
    targets[count++] = htonl(INADDR_BROADCAST);
    in_addr_t pc;
    if (inet_pton(AF_INET, host, &pc) == 1) {
        targets[count++] = (pc & htonl(0xFFFFFF00)) | htonl(0xFF);
        targets[count++] = pc;
    }
    static const uint16_t ports[] = { 9, 47009, 47998, 47999, 48000, 48002, 48010 };
    int sent = 0;
    for (int t = 0; t < count; t++)
        for (unsigned p = 0; p < sizeof ports / sizeof ports[0]; p++) {
            struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(ports[p]) };
            to.sin_addr.s_addr = targets[t];
            if (sendto(fd, packet, sizeof packet, 0, (const struct sockaddr *)&to, sizeof to) == (ssize_t)sizeof packet)
                sent++;
        }
    close(fd);
    printf("[I] wol: magic packet for %s sent %d times\n", mac_text, sent);
    return sent > 0;
}
