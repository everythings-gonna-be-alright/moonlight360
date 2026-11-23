/* Going back to devlaunch.xex: it starts programs with its own path as launch data. */
#pragma once

/* If this title was started by devlaunch, launches it back (does not return then). Otherwise returns. */
void devreturn(void);

/* The test arguments devlaunch passed after its path ("" if none), e.g. "autotest seconds=120 draw=1". */
const char *devreturn_args(void);

/* Launches path (another title beside this one) with the launch data this one got, so it returns to devlaunch in
 * turn. Does not return if the launch works. */
void devchain(const char *path);
