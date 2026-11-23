#include "diag.h"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>

#include "xsdk/kernel.h"
#include "xsdk/net.h"

/* xsdk numbers sockets from 35, up to 232 of them (xsdk src/socket_private.h). */
enum { FIRST_SOCKET_FD = 35, SOCKET_SLOTS = 232, PAGE_KB = 4 };

static int open_sockets(void)
{
    int open = 0;
    for (int fd = FIRST_SOCKET_FD; fd < FIRST_SOCKET_FD + SOCKET_SLOTS; fd++)
        if (fcntl(fd, F_GETFL) != -1)
            open++;
    return open;
}

void diag_log(const char *when)
{
    MM_QUERY_STATISTICS_RESULT mm = { .size = sizeof mm };
    NTSTATUS st = MmQueryStatistics(&mm);
    struct mallinfo heap = mallinfo();
    /* The link as XAM reports it: shows whether the (Wi-Fi) adapter dropped its link when the network dies. */
    uint32_t link = 0;
    int link_err = xsdk_net_link(&link);
    printf("[I] diag (%s): link %s (0x%02X%s)\n", when, link & XSDK_NET_LINK_ACTIVE ? "UP" : "DOWN", (unsigned)link,
           link_err ? ", query failed" : "");
    if (NT_SUCCESS(st))
        printf("[I] diag (%s): free memory title %u KB, system %u KB; heap in use %u KB; open sockets %d\n", when,
               (unsigned)(mm.title.available_pages * PAGE_KB), (unsigned)(mm.system.available_pages * PAGE_KB),
               (unsigned)(heap.uordblks / 1024), open_sockets());
    else
        printf("[I] diag (%s): heap in use %u KB; open sockets %d\n", when, (unsigned)(heap.uordblks / 1024),
               open_sockets());
}
