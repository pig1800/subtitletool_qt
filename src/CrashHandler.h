#pragma once

// Installs Windows crash handlers that, on a crash, write a stack-trace text
// log and a minidump to a "crash" folder next to the executable. Call
// install() as early as possible in main(), before any Qt work.
namespace CrashHandler {
void install();
}
