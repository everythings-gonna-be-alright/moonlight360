/* POSIX calls moonlight-common-c uses that xsdk doesn't provide. */
#include <signal.h>
#include <time.h>
#include <unistd.h>

/* The console has no signals (no SIGPIPE on a closed socket), so there is nothing to install. */
int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    (void)sig, (void)act;
    if (old) {
        old->sa_handler = SIG_DFL;
        sigemptyset(&old->sa_mask);
        old->sa_flags = 0;
    }
    return 0;
}

int usleep(useconds_t usecs)
{
    struct timespec ts = { .tv_sec = usecs / 1000000, .tv_nsec = (long)(usecs % 1000000) * 1000 };
    return nanosleep(&ts, NULL);
}
