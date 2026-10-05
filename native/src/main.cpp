// main.cpp - Entry point; enforces a single running instance.
#include <windows.h>
#include "app.h"

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    // Session-local named mutex: a second launch exits silently.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\ClaudeUsageTracker");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    int rc = runApp(hInst);
    CloseHandle(mutex);
    return rc;
}
