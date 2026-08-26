#ifndef __MILKRUN_CLI_H__
#define __MILKRUN_CLI_H__ 1

#include <windows.h>

namespace offline {

// Inspects the process command line. If it asks for a render (or for help),
// performs it, sets exitCode, and returns true; the caller should then exit
// instead of starting the interactive visualizer.
bool TryRunCommandLine(HINSTANCE hInstance, int& exitCode);

} // namespace offline

#endif
