#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static DWORD FindProcessId(const char* processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32 entry = { 0 };
    entry.dwSize = sizeof(entry);

    DWORD processId = 0;
    if (Process32First(snapshot, &entry)) {
        do {
            if (_stricmp(entry.szExeFile, processName) == 0) {
                processId = entry.th32ProcessID;
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return processId;
}

static bool BuildDefaultDllPath(char* dllPath, DWORD dllPathLen) {
    DWORD len = GetModuleFileNameA(NULL, dllPath, dllPathLen);
    if (len == 0 || len >= dllPathLen)
        return false;

    char* slash = strrchr(dllPath, '\\');
    if (!slash)
        return false;

    strcpy(slash + 1, "SpreadControlVC.dll");
    return GetFileAttributesA(dllPath) != INVALID_FILE_ATTRIBUTES;
}

static bool InjectDll(DWORD processId, const char* dllPath) {
    SIZE_T pathLen = strlen(dllPath) + 1;

    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        processId
    );
    if (!process) {
        printf("OpenProcess failed: %lu\n", GetLastError());
        return false;
    }

    LPVOID remotePath = VirtualAllocEx(process, NULL, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        printf("VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remotePath, dllPath, pathLen, NULL)) {
        printf("WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLibrary = GetProcAddress(kernel32, "LoadLibraryA");
    if (!loadLibrary) {
        printf("GetProcAddress(LoadLibraryA) failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibrary, remotePath, 0, NULL);
    if (!thread) {
        printf("CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 10000);

    DWORD remoteResult = 0;
    GetExitCodeThread(thread, &remoteResult);

    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);

    if (remoteResult == 0) {
        printf("LoadLibraryA returned NULL in the target process.\n");
        return false;
    }

    printf("Injected %s into process %lu.\n", dllPath, processId);
    return true;
}

int main(int argc, char** argv) {
    char dllPath[MAX_PATH] = { 0 };

    if (argc >= 2) {
        strncpy(dllPath, argv[1], sizeof(dllPath) - 1);
    } else if (!BuildDefaultDllPath(dllPath, sizeof(dllPath))) {
        printf("Could not find SpreadControlVC.dll beside the injector.\n");
        return 1;
    }

    DWORD processId = FindProcessId("gta-vc.exe");
    if (!processId) {
        printf("gta-vc.exe is not running. Launch the game first, then run this injector.\n");
        return 1;
    }

    return InjectDll(processId, dllPath) ? 0 : 1;
}
