#include "CrashHandler.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <csignal>
#include <cstdlib>
#include <exception>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

// Reentrancy guard so a crash inside the crash handler doesn't recurse.
static volatile LONG g_inHandler = 0;

static wchar_t g_logPath[MAX_PATH] = {};
static wchar_t g_dmpPath[MAX_PATH] = {};
static wchar_t g_crashDir[MAX_PATH] = {};

// ---- Minimal Win32 output, no CRT formatting/codec, tiny stack footprint ----

static void wbuf(HANDLE h, const char* s)
{
    // strlen is cheap and stack-light; WriteFile is a direct syscall.
    DWORD wr = 0;
    WriteFile(h, s, (DWORD)strlen(s), &wr, nullptr);
}

static void wbufn(HANDLE h, const char* s, size_t n)
{
    DWORD wr = 0;
    WriteFile(h, s, (DWORD)n, &wr, nullptr);
}

// Hand-rolled hex (no printf). buf must hold 18 chars for "0x" + 16 digits + nul.
static void hex64(char* buf, unsigned long long v)
{
    static const char* d = "0123456789abcdef";
    char tmp[16];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < 16) { tmp[n++] = d[v & 0xf]; v >>= 4; }
    buf[0] = '0'; buf[1] = 'x';
    int o = 2;
    while (n > 0) buf[o++] = tmp[--n];
    buf[o] = 0;
}

static void whex(HANDLE h, unsigned long long v)
{
    char b[20];
    hex64(b, v);
    wbuf(h, b);
}

static void wint(HANDLE h, unsigned long v)
{
    char b[12];
    if (v == 0) { wbuf(h, "0"); return; }
    char tmp[10]; int n = 0;
    while (v && n < 10) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int o = 0;
    while (n > 0) b[o++] = tmp[--n];
    b[o] = 0;
    wbuf(h, b);
}

static void buildCrashPaths()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;

    wchar_t dir[MAX_PATH];
    wcsncpy_s(dir, MAX_PATH, exe, _TRUNCATE);
    wcscat_s(dir, MAX_PATH, L"\\crash");
    wcsncpy_s(g_crashDir, MAX_PATH, dir, _TRUNCATE);
    CreateDirectoryW(g_crashDir, nullptr); // ok if it already exists

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stem[MAX_PATH];
    _snwprintf_s(stem, MAX_PATH, _TRUNCATE, L"\\crash_%04d%02d%02d_%02d%02d%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    _snwprintf_s(g_logPath, MAX_PATH, _TRUNCATE, L"%s%s.log", g_crashDir, stem);
    _snwprintf_s(g_dmpPath, MAX_PATH, _TRUNCATE, L"%s%s.dmp", g_crashDir, stem);
}

// Resolve one address to symbol+line and append. SEH-guarded by caller.
static void formatFrame(HANDLE h, HANDLE proc, int i, DWORD64 addr)
{
    char symBuf[sizeof(SYMBOL_INFO) + 512] = {};
    auto* sym = (SYMBOL_INFO*)symBuf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 512;
    DWORD64 disp = 0;
    IMAGEHLP_MODULE64 mi = {};
    mi.SizeOfStruct = sizeof(mi);
    if (SymFromAddr(proc, addr, &disp, sym)) {
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line)) {
            wbuf(h, "  ["); wint(h, i); wbuf(h, "] ");
            wbufn(h, sym->Name, sym->NameLen);
            wbuf(h, "+0x"); whex(h, disp);
            wbuf(h, "  "); wbuf(h, line.FileName);
            wbuf(h, ":"); wint(h, (unsigned long)line.LineNumber);
            wbuf(h, "  0x"); whex(h, addr); wbuf(h, "\n");
        } else {
            wbuf(h, "  ["); wint(h, i); wbuf(h, "] ");
            wbufn(h, sym->Name, sym->NameLen);
            wbuf(h, "+0x"); whex(h, disp);
            wbuf(h, "  0x"); whex(h, addr); wbuf(h, "\n");
        }
    } else if (SymGetModuleInfo64(proc, addr, &mi)) {
        wbuf(h, "  ["); wint(h, i); wbuf(h, "] ");
        wbuf(h, mi.ModuleName); wbuf(h, "+0x");
        whex(h, addr - mi.BaseOfImage);
        wbuf(h, "  0x"); whex(h, addr); wbuf(h, "\n");
    } else {
        wbuf(h, "  ["); wint(h, i); wbuf(h, "] 0x"); whex(h, addr); wbuf(h, "\n");
    }
}

// Second pass: resolve the already-captured addresses via DbgHelp. Guarded so
// a fault here can't undo the raw addresses we already wrote.
static void resolveSymbols(HANDLE h, HANDLE proc, void** frames, int n)
{
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    if (!SymInitialize(proc, nullptr, TRUE)) {
        wbuf(h, "  SymInitialize failed\n");
        return;
    }
    for (int i = 0; i < n; ++i) {
        __try {
            formatFrame(h, proc, i, (DWORD64)frames[i]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            wbuf(h, "  ["); wint(h, i); wbuf(h, "] 0x");
            whex(h, (DWORD64)frames[i]); wbuf(h, " <lookup faulted>\n");
        }
    }

    wbuf(h, "\nmodules:\n");
    HMODULE mods[256] = {};
    DWORD needed = 0;
    if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
        int count = (int)(needed / sizeof(HMODULE));
        if (count > 256) count = 256;
        for (int i = 0; i < count; ++i) {
            __try {
                wchar_t name[MAX_PATH] = {};
                if (GetModuleFileNameW(mods[i], name, MAX_PATH)) {
                    MODULEINFO mi = {};
                    if (GetModuleInformation(proc, mods[i], &mi, sizeof(mi))) {
                        char narrow[MAX_PATH] = {};
                        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow,
                                            MAX_PATH, nullptr, nullptr);
                        wbuf(h, "  base=0x"); whex(h, (DWORD64)mi.lpBaseOfDll);
                        wbuf(h, " size=0x"); wint(h, (unsigned long)mi.SizeOfImage);
                        wbuf(h, " "); wbuf(h, narrow); wbuf(h, "\n");
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    __try { SymCleanup(proc); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void writeMinidump(EXCEPTION_POINTERS* ep)
{
    __try {
        HANDLE h = CreateFileW(g_dmpPath, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                          (MINIDUMP_TYPE)(MiniDumpNormal |
                                          MiniDumpWithIndirectlyReferencedMemory |
                                          MiniDumpWithDataSegs),
                          ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void writeCrash(EXCEPTION_POINTERS* ep, const char* kind)
{
    if (InterlockedExchange(&g_inHandler, 1) != 0)
        return; // already inside the handler; bail out

    buildCrashPaths();
    HANDLE h = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        InterlockedExchange(&g_inHandler, 0);
        return;
    }

    // 1) Marker + kind + pid + exception info FIRST, with minimal stack use.
    wbuf(h, "===== subtitletool crash =====\nkind: ");
    wbuf(h, kind);
    wbuf(h, "\npid: "); wint(h, GetCurrentProcessId()); wbuf(h, "\n");

    if (ep && ep->ExceptionRecord) {
        wbuf(h, "exception code: 0x");
        whex(h, (unsigned long long)ep->ExceptionRecord->ExceptionCode);
        wbuf(h, "\nexception address: 0x");
        whex(h, (DWORD64)ep->ExceptionRecord->ExceptionAddress);
        wbuf(h, "\n");
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            wbuf(h, "access ");
            wbuf(h, ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
            wbuf(h, " at 0x");
            whex(h, (DWORD64)ep->ExceptionRecord->ExceptionInformation[1]);
            wbuf(h, "\n");
        }
    }
    wbuf(h, "--- raw stack (addresses; resolve with PDB) ---\n");

    // 2) Capture backtrace IMMEDIATELY (cheapest call) and write raw addresses.
    //    This must succeed before anything that could fault.
    void* frames[64] = {};
    int n = 0;
    if (ep && ep->ContextRecord) {
        // Walk from the faulting context so the real crashing frame is #0.
        STACKFRAME64 sf = {};
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrStack.Mode = AddrModeFlat;
        sf.AddrFrame.Mode = AddrModeFlat;
#if defined(_M_X64)
        sf.AddrPC.Offset = ep->ContextRecord->Rip;
        sf.AddrStack.Offset = ep->ContextRecord->Rsp;
        sf.AddrFrame.Offset = ep->ContextRecord->Rbp;
        const DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_IX86)
        sf.AddrPC.Offset = ep->ContextRecord->Eip;
        sf.AddrStack.Offset = ep->ContextRecord->Esp;
        sf.AddrFrame.Offset = ep->ContextRecord->Ebp;
        const DWORD machineType = IMAGE_FILE_MACHINE_I386;
#else
        const DWORD machineType = IMAGE_FILE_MACHINE_UNKNOWN;
#endif
        HANDLE proc = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();
        for (int i = 0; i < 64; ++i) {
            __try {
                if (!StackWalk64(machineType, proc, thread, &sf,
                                 ep->ContextRecord, nullptr,
                                 SymFunctionTableAccess64,
                                 SymGetModuleBase64, nullptr))
                    break;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                wbuf(h, "  <StackWalk64 faulted at frame>\n");
                break;
            }
            DWORD64 addr = sf.AddrPC.Offset;
            if (!addr) break;
            frames[n++] = (void*)addr;
            wbuf(h, "  ["); wint(h, n - 1); wbuf(h, "] 0x");
            whex(h, addr); wbuf(h, "\n");
        }
    } else {
        n = (int)RtlCaptureStackBackTrace(0, 64, frames, nullptr);
        for (int i = 0; i < n; ++i) {
            wbuf(h, "  ["); wint(h, i); wbuf(h, "] 0x");
            whex(h, (DWORD64)frames[i]); wbuf(h, "\n");
        }
    }
    wbuf(h, "--- end raw stack ---\n");

    // 3) Now attempt symbol resolution as an addendum (guarded).
    wbuf(h, "\n--- resolved (best effort) ---\n");
    __try {
        resolveSymbols(h, GetCurrentProcess(), frames, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wbuf(h, "  <symbol resolution faulted>\n");
    }

    wbuf(h, "\nlog:  "); wbuf(h, "  dump path in .dmp sibling\n");
    wbuf(h, "===== end =====\n");
    CloseHandle(h);

    writeMinidump(ep);

    InterlockedExchange(&g_inHandler, 0);
}

static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* ep)
{
    __try {
        writeCrash(ep, "UnhandledException");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return EXCEPTION_EXECUTE_HANDLER; // let the default handler terminate
}

static void terminateHandler()
{
    __try {
        writeCrash(nullptr, "std::terminate (uncaught C++ exception)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    std::abort();
}

static void __cdecl invalidParamHandler(const wchar_t*, const wchar_t*,
                                        const wchar_t*, unsigned int,
                                        uintptr_t)
{
    __try {
        writeCrash(nullptr, "CRT invalid parameter");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    std::abort();
}

static void __cdecl pureCallHandler()
{
    __try {
        writeCrash(nullptr, "pure virtual call");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    std::abort();
}

static void sigabrtHandler(int)
{
    __try {
        writeCrash(nullptr, "SIGABRT (abort/qFatal)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    std::abort();
}

namespace CrashHandler {
void install()
{
    // Reserve stack so the filter can still run if the crash was a stack
    // overflow (otherwise the handler re-faults on the exhausted stack and
    // nothing gets written). 256 KB is generous for our minimal WriteFile path.
    ULONG guard = 1 << 18; // 256 KB
    SetThreadStackGuarantee(&guard);

    SetUnhandledExceptionFilter(unhandledExceptionFilter);
    _set_invalid_parameter_handler(invalidParamHandler);
    _set_purecall_handler(pureCallHandler);
    std::set_terminate(terminateHandler);
    std::signal(SIGABRT, sigabrtHandler);
    _set_abort_behavior(0, _CALL_REPORTFAULT | _WRITE_ABORT_MSG);
}
}
