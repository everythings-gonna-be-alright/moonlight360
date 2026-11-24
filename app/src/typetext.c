#include "typetext.h"

#include <stdio.h>
#include <string.h>

#include <Limelight.h>

#include "xsdk/kernel.h"
#include "xsdk/xam.h"

enum { TEXT_CHARS = 127 };

static XAM_OVERLAPPED overlapped;
static uint16_t text[TEXT_CHARS + 1];
static bool open;

/* UTF-16 to UTF-8, surrogate pairs too; returns the bytes written (no terminator). */
static size_t to_utf8(char *out, size_t capacity, const uint16_t *in)
{
    size_t n = 0;
    while (*in) {
        uint32_t c = *in++;
        if (c >= 0xD800 && c < 0xDC00 && *in >= 0xDC00 && *in < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (*in++ - 0xDC00);
        const size_t bytes = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (n + bytes > capacity)
            break;
        if (bytes == 1) {
            out[n++] = (char)c;
        } else {
            static const uint8_t lead[5] = { 0, 0, 0xC0, 0xE0, 0xF0 };
            out[n++] = (char)(lead[bytes] | (c >> (6 * (bytes - 1))));
            for (size_t i = bytes - 1; i-- > 0;)
                out[n++] = (char)(0x80 | ((c >> (6 * i)) & 0x3F));
        }
    }
    return n;
}

bool typetext_open(uint32_t user)
{
    static const uint16_t title[] = { 'T', 'y', 'p', 'e', ' ', 'o', 'n', ' ', 't', 'h', 'e', ' ', 'P', 'C', 0 };
    static const uint16_t description[] = { 'T', 'h', 'e', ' ', 't', 'e', 'x', 't', ' ', 'g', 'o', 'e', 's', ' ',
                                            'w', 'h', 'e', 'r', 'e', ' ', 't', 'h', 'e', ' ', 'P', 'C', '\'', 's',
                                            ' ', 'c', 'u', 'r', 's', 'o', 'r', ' ', 'i', 's', '.', 0 };
    static const uint16_t empty[] = { 0 };
    if (open)
        return true;
    memset(text, 0, sizeof text);
    const uint32_t ret = xsdk_xam_keyboard(user, title, description, empty, text, TEXT_CHARS, &overlapped);
    if (ret != ERROR_IO_PENDING) {
        printf("[W] typetext: the keyboard did not open: 0x%08X\n", (unsigned)ret);
        return false;
    }
    open = true;
    return true;
}

bool typetext_poll(void)
{
    if (!open)
        return false;
    const uint32_t ret = xsdk_xam_wait(&overlapped, 0, NULL);
    if (ret == ERROR_IO_INCOMPLETE)
        return true;
    open = false;
    if (ret != ERROR_SUCCESS)
        return false;   /* cancelled */
    char utf8[TEXT_CHARS * 3 + 1];
    const size_t bytes = to_utf8(utf8, sizeof utf8, text);
    if (bytes) {
        const int err = LiSendUtf8TextEvent(utf8, (unsigned)bytes);
        printf("[I] typetext: %u bytes of text sent to the PC (%d)\n", (unsigned)bytes, err);   /* not the text */
    }
    return false;
}
