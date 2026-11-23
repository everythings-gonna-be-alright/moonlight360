#include "devreturn.h"

#include <stdio.h>
#include <string.h>

#include "xsdk/xam.h"

static char data[XSDK_LAUNCH_DATA_MAX_BYTES + 1];
static size_t data_size;
static int data_read;

static void read_data(void)
{
    if (data_read++)
        return;
    data_size = xsdk_launch_data(data, sizeof data - 1);
    data[data_size] = 0;
}

const char *devreturn_args(void)
{
    read_data();
    size_t path_length = strlen(data);
    return path_length + 1 < data_size ? data + path_length + 1 : "";
}

void devreturn(void)
{
    read_data();
    size_t length = strlen(data);
    if (length < 5 || strcmp(data + length - 4, ".xex") != 0)
        return;
    printf("[I] returning to %s\n", data);
    fflush(stdout);
    xsdk_launch(data, NULL, 0);
    printf("[W] could not return to %s\n", data);
}

void devchain(const char *path)
{
    read_data();
    printf("[I] launching %s\n", path);
    fflush(stdout);
    xsdk_launch(path, data_size ? data : NULL, (uint32_t)data_size);
    printf("[W] could not launch %s\n", path);
}
