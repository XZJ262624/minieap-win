/* Creates a hidden, isolated console exclusively for the test PowerShell and
 * probe. Never attaches to an existing application or broadcasts to its console.
 * Usage: driver <complete PowerShell command> <probe conf path> <Y or Enter>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static int has_file(const wchar_t* base, const wchar_t* suffix) {
    wchar_t path[4096];
    swprintf_s(path, 4096, L"%ls.%ls", base, suffix);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
static void key(HANDLE input, wchar_t ch) {
    INPUT_RECORD events[2] = {0};
    DWORD written;
    events[0].EventType = KEY_EVENT;
    events[0].Event.KeyEvent.bKeyDown = TRUE;
    events[0].Event.KeyEvent.wRepeatCount = 1;
    events[0].Event.KeyEvent.wVirtualKeyCode = ch == L'\r' ? VK_RETURN : (WORD)VkKeyScanW(ch);
    events[0].Event.KeyEvent.uChar.UnicodeChar = ch;
    events[1] = events[0];
    events[1].Event.KeyEvent.bKeyDown = FALSE;
    WriteConsoleInputW(input, events, 2, &written);
}
static int screen_contains(HANDLE output, const wchar_t* text) {
    wchar_t buffer[32768];
    DWORD count;
    COORD origin = {0, 0};
    if (!ReadConsoleOutputCharacterW(output, buffer, 32767, origin, &count)) return 0;
    buffer[count] = 0;
    return wcsstr(buffer, text) != NULL;
}
int wmain(int argc, wchar_t** argv) {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    HANDLE job = NULL, input = INVALID_HANDLE_VALUE, output = INVALID_HANDLE_VALUE;
    wchar_t command[8192];
    DWORD code = 0, ids[16], count, index, failure_code = 0;
    int attempts, attached = 0, found_child = 0, result = 1;
    const char* failure = "initialization";
    /* Window-manager tests use isolated hidden consoles and feed ordinary keys.
     * No control signal or broad process lookup is performed by these modes. */
    if (argc == 3 && !wcscmp(argv[1], L"--spawn")) {
        wcscpy_s(command, 8192, argv[2]);
        si.dwFlags=STARTF_USESHOWWINDOW; si.wShowWindow=SW_HIDE;
        SetConsoleCtrlHandler(NULL,FALSE);
        if(!CreateProcessW(NULL,command,NULL,NULL,FALSE,CREATE_NEW_CONSOLE,NULL,NULL,&si,&pi)) return 10;
        printf("%lu\n",pi.dwProcessId);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 0;
    }
    if (argc == 4 && !wcscmp(argv[1], L"--key")) {
        DWORD target=wcstoul(argv[2],NULL,10);
        FreeConsole();
        if(!target || !AttachConsole(target)) return 11;
        input=CreateFileW(L"CONIN$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if(input==INVALID_HANDLE_VALUE) {FreeConsole();return 12;}
        if(wcscmp(argv[3],L"Enter")) key(input,argv[3][0]);
        key(input,L'\r');
        CloseHandle(input); FreeConsole();
        return 0;
    }
    if (argc != 4) return 2;
    wcscpy_s(command, 8192, argv[1]);
    job = CreateJobObjectW(NULL, NULL);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) goto done;
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    /* Test runners may inherit CTRL_C_IGNORE. Do not pass that attribute to
     * the new PowerShell: a desktop login launcher starts with Ctrl+C enabled. */
    SetConsoleCtrlHandler(NULL, FALSE);
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NEW_CONSOLE | CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi)) goto done;
    if (!AssignProcessToJobObject(job, pi.hProcess)) { TerminateProcess(pi.hProcess, 99); goto done; }
    ResumeThread(pi.hThread);
    FreeConsole();
    for (attempts = 0; attempts < 100 && !attached; ++attempts) {
        attached = AttachConsole(pi.dwProcessId);
        if (!attached) Sleep(50);
    }
    failure = "isolated console attachment";
    if (!attached) goto done;
    count = GetConsoleProcessList(ids, 16);
    for (index = 0; index < count && index < 16; ++index) if (ids[index] == pi.dwProcessId) found_child = 1;
    if (!found_child || !SetConsoleCtrlHandler(NULL, TRUE)) goto done;
    input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    failure = "confirmation prompt";
    for (attempts = 0; attempts < 200 && !screen_contains(output, L"[Y/n]"); ++attempts) Sleep(50);
    if (attempts == 200) goto done;
    if (wcscmp(argv[3], L"Enter")) key(input, argv[3][0]);
    key(input, L'\r');
    failure = "probe startup";
    for (attempts = 0; attempts < 200 && !has_file(argv[2], L"ready"); ++attempts) Sleep(50);
    if (attempts == 200) goto done;
    Sleep(600);
    failure = "same-console UTF-8 stdout / stderr / Keep-Alive";
    if (!screen_contains(output, L"PROBE 探针认证成功") || !screen_contains(output, L"PROBE stderr") ||
        !screen_contains(output, L"PROBE Keep-Alive 2")) goto done;
    failure = "Ctrl+C delivery";
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) goto done;
    failure = "probe response to Ctrl+C";
    for (attempts = 0; attempts < 100 && !has_file(argv[2], L"stopped"); ++attempts) Sleep(50);
    if (attempts == 100) goto done;
    failure = "PowerShell exit after Ctrl+C";
    for (attempts = 0; attempts < 40 && WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT; ++attempts) {
        if (attempts % 5 == 0) key(input, L'\r');
    }
    if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) goto done;
    GetExitCodeProcess(pi.hProcess, &code);
    result = 0;
done:
    failure_code = GetLastError();
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (attached) FreeConsole();
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (job) CloseHandle(job); /* Only descendants created by THIS test. */
    if (result) fprintf(stderr, "FAIL: %s (Windows error %lu)\n", failure, failure_code);
    else printf("PASS: interactive %ls, inherited console, UTF-8, Keep-Alive, Ctrl+C; PowerShell exit=%lu\n", argv[3], code);
    return result;
}
