/* Resource figures for the log: free memory, heap use, open sockets. For tracking down leaks. */
#pragma once

/* Logs one "[I] diag (<when>): ..." line. */
void diag_log(const char *when);
