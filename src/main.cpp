#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowTitle[] = L"DeepSeek Harness 控制中心";
constexpr wchar_t kMutexName[] = L"Local\\dsh-start-control-single-instance";
constexpr wchar_t kPackageName[] = L"@deepseek-ai/dsh";
constexpr int kPort = 3080;
constexpr UINT kTimerStatus = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kAppLog = WM_APP + 2;
constexpr UINT kAppStatus = WM_APP + 3;
constexpr UINT kAppDone = WM_APP + 4;
constexpr UINT kAppVersions = WM_APP + 5;

enum ControlId : int {
    IDC_STATUS = 100,
    IDC_SUBTITLE,
    IDC_START,
    IDC_STOP,
    IDC_RESTART,
    IDC_OPEN,
    IDC_DETECT,
    IDC_CURRENT_VERSION,
    IDC_DSH_PATH,
    IDC_DSH_BROWSE,
    IDC_NPM_PATH,
    IDC_NPM_BROWSE,
    IDC_LOG_PATH,
    IDC_VERSION,
    IDC_REFRESH,
    IDC_SWITCH,
    IDC_LATEST,
    IDC_RESTORE,
    IDC_RUN_LOGIN,
    IDC_LOG,
    IDC_LIFE_GROUP,
    IDC_VERSION_GROUP,
    IDC_LOG_GROUP,
};

enum class DshState { Stopped, Starting, Running };

struct CommandResult {
    DWORD exitCode = 1;
    std::wstring stdoutText;
    std::wstring stderrText;
    bool timedOut = false;
};

struct DoneMessage {
    bool success;
    int action;
    std::wstring text;
};

struct StatusMessage {
    DshState state;
    std::wstring version;
};

struct VersionsMessage {
    std::vector<std::wstring> versions;
    std::wstring latestStable;
};

HWND g_mainWindow = nullptr;
HWND g_statusLabel = nullptr;
HWND g_titleLabel = nullptr;
HWND g_currentVersion = nullptr;
HWND g_currentVersionName = nullptr;
HWND g_dshPathEdit = nullptr;
HWND g_dshPathName = nullptr;
HWND g_npmPathEdit = nullptr;
HWND g_npmPathName = nullptr;
HWND g_versionCombo = nullptr;
HWND g_versionName = nullptr;
HWND g_latestLabel = nullptr;
HWND g_logPathLabel = nullptr;
HWND g_logPathName = nullptr;
HWND g_logEdit = nullptr;
HWND g_startButton = nullptr;
HWND g_stopButton = nullptr;
HWND g_restartButton = nullptr;
HWND g_openButton = nullptr;
HWND g_detectButton = nullptr;
HWND g_refreshButton = nullptr;
HWND g_switchButton = nullptr;
HWND g_restoreCheck = nullptr;
HWND g_loginCheck = nullptr;
HBRUSH g_windowBrush = nullptr;
HFONT g_font = nullptr;
HFONT g_boldFont = nullptr;
HFONT g_titleFont = nullptr;
HANDLE g_instanceMutex = nullptr;
HANDLE g_hostProcess = nullptr;
std::atomic<DWORD> g_hostPid{0};
std::atomic<bool> g_operationBusy{false};
std::atomic<bool> g_statusBusy{false};
std::atomic<bool> g_versionsBusy{false};
std::atomic<bool> g_shuttingDown{false};
std::mutex g_processMutex;
std::mutex g_logMutex;
std::wstring g_dshPath;
std::wstring g_npmPath;
std::wstring g_webUrl;
std::wstring g_logPath;
std::vector<std::wstring> g_versions;

void Log(const std::wstring& message);

std::wstring LocalAppData() {
    wchar_t buffer[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) return buffer;
    return L".";
}

std::wstring SettingsPath() { return LocalAppData() + L"\\dsh-start-control\\settings.json"; }

std::wstring LogDirectory() { return LocalAppData() + L"\\dsh-start-control\\logs"; }

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) size = MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) <= 0)
        MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring Trim(std::wstring value) {
    const auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!value.empty() && isSpace(value.front())) value.erase(value.begin());
    while (!value.empty() && isSpace(value.back())) value.pop_back();
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::wstring QuoteArg(const std::wstring& value) {
    std::wstring result = L"\"";
    for (wchar_t c : value) {
        if (c == L'"') result += L'\\';
        result += c;
    }
    result += L'"';
    return result;
}

std::wstring CommandLineFor(const std::wstring& command, const std::vector<std::wstring>& args, std::wstring& application) {
    const bool isCmd = ToLower(fs::path(command).extension().wstring()) == L".cmd" ||
                       ToLower(fs::path(command).extension().wstring()) == L".bat";
    if (isCmd) {
        wchar_t comspec[MAX_PATH]{};
        GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
        application = comspec[0] == L'\0' ? L"C:\\Windows\\System32\\cmd.exe" : comspec;
        std::wstring line = QuoteArg(application) + L" /d /c call " + QuoteArg(command);
        for (const auto& arg : args) line += L" " + QuoteArg(arg);
        return line;
    }
    // Let CreateProcess resolve ordinary executables through PATH. Passing a bare
    // name as lpApplicationName is not consistent for system tools such as taskkill.
    application.clear();
    std::wstring line = QuoteArg(command);
    for (const auto& arg : args) line += L" " + QuoteArg(arg);
    return line;
}

CommandResult RunCommand(const std::wstring& command, const std::vector<std::wstring>& args, DWORD timeoutMs, bool logOutput = false) {
    CommandResult result;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        result.stderrText = L"无法创建命令输出管道。";
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring application;
    std::wstring commandLine = CommandLineFor(command, args, application);
    std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
    mutableLine.push_back(L'\0');
    STARTUPINFOW startup{sizeof(STARTUPINFOW)};
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(application.empty() ? nullptr : application.c_str(), mutableLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &processInfo)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        result.stderrText = L"无法启动命令：" + command + L"（错误码 " + std::to_wstring(GetLastError()) + L"）。";
        return result;
    }
    CloseHandle(writePipe);

    std::string output;
    char buffer[4096];
    DWORD read = 0;
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &read, nullptr) && read > 0) {
            DWORD bytes = 0;
            if (!ReadFile(readPipe, buffer, sizeof(buffer), &bytes, nullptr) || bytes == 0) break;
            output.append(buffer, buffer + bytes);
        }
        DWORD wait = WaitForSingleObject(processInfo.hProcess, 50);
        if (wait == WAIT_OBJECT_0) {
            while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) output.append(buffer, buffer + read);
            break;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (static_cast<DWORD>(elapsed) >= timeoutMs) {
            TerminateProcess(processInfo.hProcess, 124);
            result.timedOut = true;
            break;
        }
    }
    GetExitCodeProcess(processInfo.hProcess, &result.exitCode);
    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);
    CloseHandle(readPipe);

    result.stdoutText = FromUtf8(output);
    if (logOutput) {
        std::wistringstream lines(result.stdoutText);
        std::wstring line;
        while (std::getline(lines, line)) {
            if (!Trim(line).empty()) {
                Log(Trim(line));
            }
        }
    }
    return result;
}

bool IsSemVer(const std::wstring& value) {
    static const std::wregex pattern(LR"(^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$)");
    return std::regex_match(value, pattern);
}

struct SemVerParts {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::wstring pre;
};

SemVerParts ParseSemVer(const std::wstring& value) {
    SemVerParts parts;
    wchar_t separator = L'\0';
    std::wistringstream stream(value);
    stream >> parts.major >> separator >> parts.minor >> separator >> parts.patch;
    const auto dash = value.find(L'-');
    if (dash != std::wstring::npos) parts.pre = value.substr(dash + 1);
    return parts;
}

int CompareSemVer(const std::wstring& left, const std::wstring& right) {
    const auto a = ParseSemVer(left);
    const auto b = ParseSemVer(right);
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre.empty() != b.pre.empty()) return a.pre.empty() ? 1 : -1;
    if (a.pre == b.pre) return 0;
    return a.pre < b.pre ? -1 : 1;
}

std::wstring FindCommand(const std::wstring& name) {
    const auto result = RunCommand(L"where.exe", {name}, 5000);
    std::wistringstream lines(result.stdoutText);
    std::wstring line;
    std::wstring first;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.empty() || !fs::exists(line)) continue;
        if (first.empty()) first = line;
        if (ToLower(fs::path(line).extension().wstring()) == L".cmd") return line;
    }
    return first;
}

bool IsCommandPathUsable(const std::wstring& path, const wchar_t* expectedFileName) {
    if (path.empty()) return false;
    std::error_code error;
    if (!fs::is_regular_file(fs::path(path), error)) return false;
    return _wcsicmp(fs::path(path).filename().c_str(), expectedFileName) == 0;
}

std::wstring ReadTextControl(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(control, value.data(), length + 1);
        value.resize(static_cast<size_t>(length));
    } else {
        value.clear();
    }
    return Trim(value);
}

void WriteTextControl(HWND control, const std::wstring& value) { SetWindowTextW(control, value.c_str()); }

void EnsureLogDirectory() {
    std::error_code error;
    fs::create_directories(LogDirectory(), error);
}

void WriteLogFile(const std::wstring& message) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    EnsureLogDirectory();
    HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t prefix[64]{};
    swprintf_s(prefix, L"[%02u:%02u:%02u] ", now.wHour, now.wMinute, now.wSecond);
    const std::string utf8 = ToUtf8(std::wstring(prefix) + message + L"\r\n");
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

void Log(const std::wstring& message) {
    WriteLogFile(message);
    if (g_mainWindow && !g_shuttingDown) {
        PostMessageW(g_mainWindow, kAppLog, 0, reinterpret_cast<LPARAM>(new std::wstring(message)));
    }
}

std::wstring ReadLastUrlFromLog() {
    if (!fs::exists(g_logPath)) return {};
    std::ifstream file(fs::path(g_logPath), std::ios::binary);
    if (!file) return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::wstring text = FromUtf8(buffer.str());
    static const std::wregex pattern(LR"((https?://(?:127\.0\.0\.1|localhost):[0-9]+/\?token=\S+))", std::regex_constants::icase);
    std::wsregex_iterator it(text.begin(), text.end(), pattern);
    std::wsregex_iterator end;
    std::wstring last;
    for (; it != end; ++it) last = (*it)[1].str();
    while (!last.empty() && (last.back() == L'.' || last.back() == L',' || last.back() == L';' || last.back() == L')')) last.pop_back();
    return last;
}

std::vector<DWORD> ListeningPids() {
    std::vector<DWORD> pids;
    DWORD tableSize = 0;
    if (GetExtendedTcpTable(nullptr, &tableSize, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER) return pids;
    std::vector<BYTE> tableBuffer(tableSize);
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(tableBuffer.data());
    if (GetExtendedTcpTable(table, &tableSize, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return pids;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (ntohs(static_cast<u_short>(row.dwLocalPort)) == kPort && row.dwOwningPid != 0 &&
            std::find(pids.begin(), pids.end(), row.dwOwningPid) == pids.end()) {
            pids.push_back(row.dwOwningPid);
        }
    }
    return pids;
}

bool IsNodeProcess(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH * 4]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!ok) return false;
    const std::wstring name = ToLower(fs::path(path).filename().wstring());
    return name == L"node.exe" || name == L"nodejs.exe";
}

DshState GetStateInternal() {
    if (!ListeningPids().empty()) return DshState::Running;
    if (g_hostProcess && WaitForSingleObject(g_hostProcess, 0) == WAIT_TIMEOUT) return DshState::Starting;
    return DshState::Stopped;
}

void CloseHostHandle() {
    if (g_hostProcess) {
        CloseHandle(g_hostProcess);
        g_hostProcess = nullptr;
    }
    g_hostPid = 0;
}

bool StartInternal(std::wstring& message) {
    std::lock_guard<std::mutex> lock(g_processMutex);
    if (GetStateInternal() == DshState::Running) {
        message = L"DSH Web 已经在运行中。";
        return true;
    }
    if (!IsCommandPathUsable(g_dshPath, L"dsh.cmd")) {
        message = L"找不到 dsh.cmd，请检查命令路径或点击“自动检测路径”。";
        return false;
    }
    EnsureLogDirectory();
    g_webUrl.clear();
    STARTUPINFOW startup{sizeof(STARTUPINFOW)};
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        message = L"无法创建 dsh 日志管道。";
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    std::wstring application;
    const std::wstring line = CommandLineFor(g_dshPath, {L"web", L"--no-open"}, application);
    std::vector<wchar_t> mutableLine(line.begin(), line.end());
    mutableLine.push_back(L'\0');
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(application.c_str(), mutableLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, LocalAppData().c_str(), &startup, &info)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        message = L"无法启动 dsh 进程，错误码：" + std::to_wstring(GetLastError());
        return false;
    }
    CloseHandle(writePipe);
    CloseHostHandle();
    g_hostProcess = info.hProcess;
    g_hostPid = info.dwProcessId;
    HANDLE readerProcess = nullptr;
    DuplicateHandle(GetCurrentProcess(), info.hProcess, GetCurrentProcess(), &readerProcess, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(info.hThread);
    Log(L"已在后台启动 dsh web，管理 PID：" + std::to_wstring(info.dwProcessId) + L"。");

    std::thread([readPipe, process = readerProcess]() {
        char buffer[4096];
        DWORD bytes = 0;
        std::string pending;
        while (!g_shuttingDown && ReadFile(readPipe, buffer, sizeof(buffer), &bytes, nullptr) && bytes > 0) {
            pending.append(buffer, buffer + bytes);
            size_t newline = 0;
            while ((newline = pending.find_first_of("\r\n")) != std::string::npos) {
                const std::string lineText = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!lineText.empty()) Log(FromUtf8(lineText));
            }
        }
        if (!pending.empty()) Log(FromUtf8(pending));
        CloseHandle(readPipe);
        DWORD code = 0;
        if (GetExitCodeProcess(process, &code) && !g_shuttingDown) Log(L"dsh 进程已退出，退出码：" + std::to_wstring(code) + L"。");
        if (process) CloseHandle(process);
    }).detach();

    for (int i = 0; i < 60; ++i) {
        if (!ListeningPids().empty()) {
            message = L"DSH Web 已启动，正在监听 http://127.0.0.1:3080。";
            return true;
        }
        if (WaitForSingleObject(g_hostProcess, 0) == WAIT_OBJECT_0) {
            message = L"DSH 启动失败，进程已退出，请查看运行日志。";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    message = L"DSH 启动超时，请查看运行日志。";
    return false;
}

bool StopInternal(std::wstring& message) {
    std::lock_guard<std::mutex> lock(g_processMutex);
    std::vector<DWORD> pids = ListeningPids();
    const DWORD hostPid = g_hostPid.load();
    if (hostPid != 0 && std::find(pids.begin(), pids.end(), hostPid) == pids.end()) pids.push_back(hostPid);
    if (pids.empty()) {
        message = L"DSH Web 当前没有运行。";
        return true;
    }
    bool stopped = false;
    for (const DWORD pid : pids) {
        if (pid != hostPid && !IsNodeProcess(pid)) {
            Log(L"端口 3080 被 PID " + std::to_wstring(pid) + L" 占用，但未确认是 Node/DSH，已跳过。 ");
            continue;
        }
        const auto result = RunCommand(L"taskkill.exe", {L"/PID", std::to_wstring(pid), L"/T", L"/F"}, 10000, true);
        if (result.exitCode == 0) {
            stopped = true;
            Log(L"已结束 DSH 进程树 PID " + std::to_wstring(pid) + L"。 ");
        }
    }
    for (int i = 0; i < 20 && !ListeningPids().empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (ListeningPids().empty()) {
        CloseHostHandle();
        message = stopped ? L"DSH Web 已停止。" : L"没有结束任何 DSH 进程。";
        return stopped;
    }
    message = L"DSH 仍占用 3080 端口，请查看日志或确认端口占用进程。";
    return false;
}

bool RestartInternal(std::wstring& message) {
    std::wstring ignored;
    if (!StopInternal(ignored) && GetStateInternal() != DshState::Stopped) {
        message = ignored;
        return false;
    }
    return StartInternal(message);
}

bool InstallVersionInternal(const std::wstring& version, std::wstring& message) {
    if (!IsSemVer(version)) {
        message = L"版本号格式无效。";
        return false;
    }
    if (!IsCommandPathUsable(g_npmPath, L"npm.cmd")) {
        message = L"找不到 npm.cmd，请检查命令路径或点击“自动检测路径”。";
        return false;
    }
    Log(L"执行全局安装：" + std::wstring(kPackageName) + L"@" + version);
    const auto result = RunCommand(g_npmPath, {L"install", L"--global", std::wstring(kPackageName) + L"@" + version, L"--save-exact"}, 180000, true);
    if (result.exitCode != 0) {
        message = L"npm 安装失败（退出码 " + std::to_wstring(result.exitCode) + L"），请查看运行日志。";
        return false;
    }
    const auto detected = FindCommand(L"dsh");
    if (!detected.empty()) g_dshPath = detected;
    message = L"DSH 已切换到 " + version + L"。";
    return true;
}

void PostLog(const std::wstring& message) {
    if (g_mainWindow) PostMessageW(g_mainWindow, kAppLog, 0, reinterpret_cast<LPARAM>(new std::wstring(message)));
}

void PostDone(bool success, int action, const std::wstring& message) {
    if (g_mainWindow) PostMessageW(g_mainWindow, kAppDone, 0, reinterpret_cast<LPARAM>(new DoneMessage{success, action, message}));
}

void BeginOperation(const std::function<void()>& work) {
    bool expected = false;
    if (!g_operationBusy.compare_exchange_strong(expected, true)) return;
    EnableWindow(g_startButton, FALSE);
    EnableWindow(g_stopButton, FALSE);
    EnableWindow(g_restartButton, FALSE);
    EnableWindow(g_openButton, FALSE);
    EnableWindow(g_detectButton, FALSE);
    EnableWindow(g_refreshButton, FALSE);
    EnableWindow(g_switchButton, FALSE);
    EnableWindow(g_dshPathEdit, FALSE);
    EnableWindow(g_npmPathEdit, FALSE);
    std::thread([work]() { work(); }).detach();
}

void UpdateStatusLabel(DshState state) {
    const wchar_t* text = state == DshState::Running ? L"DSH Web 状态：运行中" : state == DshState::Starting ? L"DSH Web 状态：启动中" : L"DSH Web 状态：已停止";
    SetWindowTextW(g_statusLabel, text);
    RedrawWindow(g_statusLabel, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
    EnableWindow(g_startButton, state == DshState::Stopped && !g_operationBusy);
    EnableWindow(g_stopButton, state != DshState::Stopped && !g_operationBusy);
    EnableWindow(g_restartButton, state != DshState::Starting && !g_operationBusy);
    EnableWindow(g_openButton, state != DshState::Stopped && !g_operationBusy);
}

void RefreshStatusInBackground() {
    bool expected = false;
    if (!g_statusBusy.compare_exchange_strong(expected, true)) return;
    std::thread([]() {
        const DshState state = GetStateInternal();
        std::wstring version;
        if (!g_dshPath.empty()) {
            const auto result = RunCommand(g_dshPath, {L"--version"}, 10000);
            if (result.exitCode == 0) {
                static const std::wregex pattern(LR"([0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?)");
                std::wsmatch match;
                const std::wstring output = result.stdoutText + L"\n" + result.stderrText;
                if (std::regex_search(output, match, pattern)) version = match.str();
            }
        }
        if (g_mainWindow) PostMessageW(g_mainWindow, kAppStatus, 0, reinterpret_cast<LPARAM>(new StatusMessage{state, version}));
    }).detach();
}

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring result;
    for (wchar_t c : value) {
        if (c == L'\\') result += L"\\\\";
        else if (c == L'"') result += L"\\\"";
        else if (c == L'\r') result += L"\\r";
        else if (c == L'\n') result += L"\\n";
        else result += c;
    }
    return result;
}

std::wstring LoadSetting(const std::wstring& key) {
    std::ifstream file(fs::path(SettingsPath()), std::ios::binary);
    if (!file) return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::wstring text = FromUtf8(buffer.str());
    const std::wstring expression = L"\"" + key + L"\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"";
    std::wregex pattern(expression);
    std::wsmatch match;
    if (!std::regex_search(text, match, pattern)) return {};
    std::wstring result = match[1].str();
    std::wstring unescaped;
    bool escaped = false;
    for (wchar_t c : result) {
        if (escaped) {
            unescaped += c == L'\\' ? L'\\' : c;
            escaped = false;
        } else if (c == L'\\') escaped = true;
        else unescaped += c;
    }
    return unescaped;
}

void SaveSettings() {
    const std::wstring dsh = ReadTextControl(g_dshPathEdit);
    const std::wstring npm = ReadTextControl(g_npmPathEdit);
    g_dshPath = dsh;
    g_npmPath = npm;
    std::error_code error;
    fs::create_directories(fs::path(SettingsPath()).parent_path(), error);
    const std::wstring json = L"{\n  \"DshPath\": \"" + JsonEscape(dsh) + L"\",\n  \"NpmPath\": \"" + JsonEscape(npm) + L"\"\n}\n";
    std::ofstream file(fs::path(SettingsPath()), std::ios::binary | std::ios::trunc);
    if (file) file << ToUtf8(json);
}

void ApplyPathsFromUi() {
    g_dshPath = ReadTextControl(g_dshPathEdit);
    g_npmPath = ReadTextControl(g_npmPathEdit);
    SaveSettings();
}

void DetectPaths(bool refreshVersions) {
    BeginOperation([refreshVersions]() {
        const auto dsh = FindCommand(L"dsh");
        const auto npm = FindCommand(L"npm");
        g_dshPath = dsh;
        g_npmPath = npm;
        Log(dsh.empty() ? L"自动检测失败：未找到 dsh.cmd。" : L"已自动检测到 dsh.cmd：" + dsh);
        Log(npm.empty() ? L"自动检测失败：未找到 npm.cmd。" : L"已自动检测到 npm.cmd：" + npm);
        PostDone(true, refreshVersions ? 10 : 11, L"命令路径检测完成。");
    });
}

void RefreshVersions() {
    ApplyPathsFromUi();
    if (!IsCommandPathUsable(g_npmPath, L"npm.cmd")) {
        MessageBoxW(g_mainWindow, L"未找到 npm.cmd，请先输入 npm.cmd 的完整路径，或点击“自动检测路径”。", L"npm 路径无效", MB_OK | MB_ICONWARNING);
        return;
    }
    bool expected = false;
    if (!g_versionsBusy.compare_exchange_strong(expected, true)) return;
    EnableWindow(g_refreshButton, FALSE);
    BeginOperation([]() {
        Log(L"正在从 npm 检查 @deepseek-ai/dsh 版本……");
        const auto result = RunCommand(g_npmPath, {L"view", std::wstring(kPackageName), L"versions", L"--json"}, 180000, false);
        if (result.exitCode != 0) {
            Log(L"npm 版本检查失败：" + (result.stderrText.empty() ? result.stdoutText : result.stderrText));
            g_versionsBusy = false;
            PostDone(false, 0, L"npm 版本检查失败，请查看运行日志。 ");
            return;
        }
        static const std::wregex versionPattern(LR"(\"([0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?)\")");
        std::vector<std::wstring> versions;
        std::unordered_set<std::wstring> unique;
        for (std::wsregex_iterator it(result.stdoutText.begin(), result.stdoutText.end(), versionPattern), end; it != end; ++it) {
            const std::wstring version = (*it)[1].str();
            const std::wstring key = ToLower(version);
            if (IsSemVer(version) && unique.insert(key).second) versions.push_back(version);
        }
        std::sort(versions.begin(), versions.end(), [](const auto& a, const auto& b) { return CompareSemVer(a, b) > 0; });
        std::wstring stable;
        for (const auto& version : versions) {
            if (version.find(L'-') == std::wstring::npos) { stable = version; break; }
        }
        g_versionsBusy = false;
        if (g_mainWindow) PostMessageW(g_mainWindow, kAppVersions, 0, reinterpret_cast<LPARAM>(new VersionsMessage{std::move(versions), stable}));
        PostDone(true, 0, L"npm 版本检查完成。 ");
    });
}

void SwitchVersion() {
    const int index = static_cast<int>(SendMessageW(g_versionCombo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(g_versions.size())) {
        MessageBoxW(g_mainWindow, L"请先刷新版本列表并选择目标版本。", L"未选择版本", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring target = g_versions[index];
    const int answer = MessageBoxW(g_mainWindow,
        (L"即将把全局 DSH 切换到 " + target + L"。\n\n操作会先停止 DSH Web，再通过 npm 全局安装目标版本；现有会话和 profile 不会被删除。\n\n继续吗？").c_str(),
        L"确认 DSH 版本切换", MB_YESNO | MB_ICONQUESTION);
    if (answer != IDYES) return;
    ApplyPathsFromUi();
    const bool restore = SendMessageW(g_restoreCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    BeginOperation([target, restore]() {
        const bool wasRunning = GetStateInternal() != DshState::Stopped;
        std::wstring message;
        if (!StopInternal(message) && GetStateInternal() != DshState::Stopped) {
            PostDone(false, 0, message);
            return;
        }
        if (!InstallVersionInternal(target, message)) {
            PostDone(false, 0, message);
            return;
        }
        if (restore && wasRunning) {
            std::wstring restartMessage;
            if (!StartInternal(restartMessage)) {
                PostDone(false, 0, L"版本切换成功，但恢复 DSH Web 失败：" + restartMessage);
                return;
            }
            message += L" 已按切换前状态恢复运行。";
        }
        PostDone(true, 0, message);
    });
}

void OpenWeb() {
    std::wstring url = g_webUrl.empty() ? ReadLastUrlFromLog() : g_webUrl;
    if (url.empty()) {
        Log(L"没有找到带访问令牌的 DSH Web 地址，请先启动或重启 DSH Web。 ");
        return;
    }
    ShellExecuteW(g_mainWindow, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void BrowseCommand(HWND target, const wchar_t* title) {
    wchar_t fileName[MAX_PATH * 4]{};
    const std::wstring current = ReadTextControl(target);
    if (!current.empty()) wcsncpy_s(fileName, current.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
    dialog.hwndOwner = g_mainWindow;
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = static_cast<DWORD>(std::size(fileName));
    dialog.lpstrTitle = title;
    dialog.lpstrFilter = L"命令文件 (*.cmd)\0*.cmd\0所有文件 (*.*)\0*.*\0\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog)) {
        WriteTextControl(target, fileName);
        ApplyPathsFromUi();
    }
}

void SetRunAtLogin(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        Log(L"无法修改 Windows 登录启动设置。 ");
        return;
    }
    wchar_t executable[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    const std::wstring command = QuoteArg(executable);
    const LONG status = enabled ? RegSetValueExW(key, L"dsh-start-control", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) : RegDeleteValueW(key, L"dsh-start-control");
    RegCloseKey(key);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) Log(L"更新 Windows 登录启动设置失败。 ");
}

bool IsRunAtLogin() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    wchar_t value[MAX_PATH * 4]{};
    DWORD size = sizeof(value);
    const bool present = RegQueryValueExW(key, L"dsh-start-control", nullptr, nullptr, reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return present;
}

void SetControlFont(HWND control, HFONT font) { if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); }

HWND AddControl(const wchar_t* className, const wchar_t* text, DWORD style, int id, HWND parent) {
    HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SetControlFont(control, g_font);
    return control;
}

int Scale(int value) {
    const UINT dpi = g_mainWindow ? GetDpiForWindow(g_mainWindow) : 96;
    return MulDiv(value, static_cast<int>(dpi), 96);
}

void MoveControl(HWND control, int x, int y, int width, int height) { if (control) MoveWindow(control, Scale(x), Scale(y), Scale(width), Scale(height), TRUE); }

void CreateUi(HWND hwnd) {
    g_statusLabel = AddControl(L"STATIC", L"DSH Web 状态：检测中", SS_RIGHT | SS_CENTERIMAGE, IDC_STATUS, hwnd);
    g_statusLabel = g_statusLabel;
    g_titleLabel = AddControl(L"STATIC", L"DSH Web 控制中心", SS_LEFT | SS_CENTERIMAGE, 0, hwnd);
    SetControlFont(g_titleLabel, g_titleFont);
    AddControl(L"STATIC", L"管理本机 DSH Web 服务。关闭窗口会隐藏到托盘，可从托盘继续启动、停止或重启。", SS_LEFT | SS_CENTERIMAGE, IDC_SUBTITLE, hwnd);

    AddControl(L"BUTTON", L"DSH Web 服务控制", BS_GROUPBOX, IDC_LIFE_GROUP, hwnd);
    g_startButton = AddControl(L"BUTTON", L"启动 DSH Web", BS_PUSHBUTTON, IDC_START, hwnd);
    g_stopButton = AddControl(L"BUTTON", L"停止 DSH Web", BS_PUSHBUTTON, IDC_STOP, hwnd);
    g_restartButton = AddControl(L"BUTTON", L"重启 DSH Web", BS_PUSHBUTTON, IDC_RESTART, hwnd);
    g_openButton = AddControl(L"BUTTON", L"打开 DSH 网页", BS_PUSHBUTTON, IDC_OPEN, hwnd);
    g_detectButton = AddControl(L"BUTTON", L"自动检测路径", BS_PUSHBUTTON, IDC_DETECT, hwnd);
    g_currentVersionName = AddControl(L"STATIC", L"当前 DSH 版本", SS_LEFT | SS_CENTERIMAGE, 0, hwnd);
    g_currentVersion = AddControl(L"STATIC", L"—", SS_LEFT | SS_CENTERIMAGE, IDC_CURRENT_VERSION, hwnd);
    g_dshPathName = AddControl(L"STATIC", L"dsh.cmd 位置", SS_LEFT | SS_CENTERIMAGE, 0, hwnd);
    g_dshPathEdit = AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, IDC_DSH_PATH, hwnd);
    HWND dshBrowse = AddControl(L"BUTTON", L"选择…", BS_PUSHBUTTON, IDC_DSH_BROWSE, hwnd);
    g_npmPathName = AddControl(L"STATIC", L"npm.cmd 位置", SS_LEFT | SS_CENTERIMAGE, 0, hwnd);
    g_npmPathEdit = AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, IDC_NPM_PATH, hwnd);
    HWND npmBrowse = AddControl(L"BUTTON", L"选择…", BS_PUSHBUTTON, IDC_NPM_BROWSE, hwnd);
    g_logPathName = AddControl(L"STATIC", L"日志位置", SS_LEFT | SS_CENTERIMAGE, 0, hwnd);
    g_logPathLabel = AddControl(L"STATIC", L"—", SS_LEFT | SS_CENTERIMAGE, IDC_LOG_PATH, hwnd);

    AddControl(L"BUTTON", L"DSH 版本切换（升级 / 回退）", BS_GROUPBOX, IDC_VERSION_GROUP, hwnd);
    g_versionName = AddControl(L"STATIC", L"目标 DSH 版本", SS_LEFT | SS_CENTERIMAGE, 0, hwnd);
    g_versionCombo = AddControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_VERSION, hwnd);
    g_refreshButton = AddControl(L"BUTTON", L"刷新版本列表", BS_PUSHBUTTON, IDC_REFRESH, hwnd);
    g_switchButton = AddControl(L"BUTTON", L"切换到选中版本", BS_PUSHBUTTON, IDC_SWITCH, hwnd);
    g_latestLabel = AddControl(L"STATIC", L"最新稳定版：—", SS_LEFT | SS_CENTERIMAGE, IDC_LATEST, hwnd);
    g_restoreCheck = AddControl(L"BUTTON", L"切换成功后恢复切换前的运行状态", BS_AUTOCHECKBOX, IDC_RESTORE, hwnd);
    g_loginCheck = AddControl(L"BUTTON", L"登录 Windows 后自动打开控制中心", BS_AUTOCHECKBOX, IDC_RUN_LOGIN, hwnd);
    SendMessageW(g_restoreCheck, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g_loginCheck, BM_SETCHECK, IsRunAtLogin() ? BST_CHECKED : BST_UNCHECKED, 0);

    AddControl(L"BUTTON", L"运行日志", BS_GROUPBOX, IDC_LOG_GROUP, hwnd);
    g_logEdit = AddControl(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | WS_BORDER, IDC_LOG, hwnd);
    SendMessageW(g_logEdit, EM_SETLIMITTEXT, 1024 * 1024, 0);
    EnableWindow(g_stopButton, FALSE);
    EnableWindow(g_restartButton, FALSE);
    EnableWindow(g_openButton, FALSE);

    const std::wstring savedDsh = LoadSetting(L"DshPath");
    const std::wstring savedNpm = LoadSetting(L"NpmPath");
    WriteTextControl(g_dshPathEdit, savedDsh);
    WriteTextControl(g_npmPathEdit, savedNpm);
    g_dshPath = savedDsh;
    g_npmPath = savedNpm;
    WriteTextControl(g_logPathLabel, g_logPath);
    WriteTextControl(g_currentVersion, L"读取中…");

    SetTimer(hwnd, kTimerStatus, 2000, nullptr);
    if (!IsCommandPathUsable(g_dshPath, L"dsh.cmd") || !IsCommandPathUsable(g_npmPath, L"npm.cmd")) DetectPaths(true);
    else RefreshVersions();
    RefreshStatusInBackground();
    (void)dshBrowse;
    (void)npmBrowse;
}

void LayoutUi(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = static_cast<int>(client.right - client.left) * 96 / static_cast<int>(GetDpiForWindow(hwnd));
    const int height = static_cast<int>(client.bottom - client.top) * 96 / static_cast<int>(GetDpiForWindow(hwnd));
    const int right = std::max(780, width - 24);
    const int contentWidth = right - 24;
    MoveControl(g_titleLabel, 24, 18, 360, 34);
    MoveControl(g_statusLabel, 560, 18, std::max(200, width - 580), 34);
    RedrawWindow(g_statusLabel, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
    MoveControl(GetDlgItem(hwnd, IDC_SUBTITLE), 24, 52, contentWidth, 26);
    MoveControl(GetDlgItem(hwnd, IDC_LIFE_GROUP), 18, 78, contentWidth + 12, 182);
    MoveControl(g_startButton, 38, 104, 116, 30);
    MoveControl(g_stopButton, 162, 104, 116, 30);
    MoveControl(g_restartButton, 286, 104, 116, 30);
    MoveControl(g_openButton, 410, 104, 126, 30);
    MoveControl(g_detectButton, 544, 104, 112, 30);
    const int valueX = 160;
    MoveControl(g_currentVersionName, 38, 140, 112, 26);
    MoveControl(g_currentVersion, valueX, 140, contentWidth - valueX - 14, 26);
    MoveControl(g_dshPathName, 38, 169, 112, 26);
    MoveControl(g_dshPathEdit, valueX, 169, contentWidth - valueX - 92, 26);
    MoveControl(GetDlgItem(hwnd, IDC_DSH_BROWSE), width - 102, 169, 70, 26);
    MoveControl(g_npmPathName, 38, 198, 112, 26);
    MoveControl(g_npmPathEdit, valueX, 198, contentWidth - valueX - 92, 26);
    MoveControl(GetDlgItem(hwnd, IDC_NPM_BROWSE), width - 102, 198, 70, 26);
    MoveControl(g_logPathName, 38, 227, 112, 26);
    MoveControl(g_logPathLabel, valueX, 227, contentWidth - valueX - 14, 26);

    MoveControl(GetDlgItem(hwnd, IDC_VERSION_GROUP), 18, 270, contentWidth + 12, 126);
    MoveControl(g_versionName, 38, 296, 102, 28);
    const int switchX = width - 234;
    const int refreshX = switchX - 10 - 126;
    const int comboWidth = std::max(180, refreshX - 10 - 148);
    MoveControl(g_versionCombo, 148, 296, comboWidth, 28);
    MoveControl(g_refreshButton, refreshX, 296, 126, 30);
    MoveControl(g_switchButton, switchX, 296, 194, 30);
    MoveControl(g_latestLabel, 148, 328, 360, 28);
    MoveControl(g_restoreCheck, 520, 328, std::max(220, width - 560), 28);
    MoveControl(g_loginCheck, 38, 360, 430, 28);
    MoveControl(GetDlgItem(hwnd, IDC_LOG_GROUP), 18, 406, contentWidth + 12, std::max(110, height - 416));
    MoveControl(g_logEdit, 28, 428, contentWidth - 8, std::max(78, height - 440));
}

void AppendLogToEdit(const std::wstring& message) {
    const std::wstring line = message + L"\r\n";
    const int length = GetWindowTextLengthW(g_logEdit);
    SendMessageW(g_logEdit, EM_SETSEL, length, length);
    SendMessageW(g_logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(g_logEdit, EM_SCROLLCARET, 0, 0);
}

void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW data{sizeof(NOTIFYICONDATAW)};
    data.hWnd = hwnd;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    wcsncpy_s(data.szTip, kWindowTitle, _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &data);
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW data{sizeof(NOTIFYICONDATAW)};
    data.hWnd = hwnd;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 2001, L"打开控制中心");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2002, L"启动 DSH Web");
    AppendMenuW(menu, MF_STRING, 2003, L"停止 DSH Web");
    AppendMenuW(menu, MF_STRING, 2004, L"重启 DSH Web");
    AppendMenuW(menu, MF_STRING, 2005, L"打开 DSH 网页");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2006, L"退出控制中心");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(hwnd);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    switch (command) {
    case 2001: ShowWindow(hwnd, SW_SHOW); ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); break;
    case 2002: ApplyPathsFromUi(); BeginOperation([]() { std::wstring message; const bool ok = StartInternal(message); PostDone(ok, 0, message); }); break;
    case 2003: BeginOperation([]() { std::wstring message; const bool ok = StopInternal(message); PostDone(ok, 0, message); }); break;
    case 2004: ApplyPathsFromUi(); BeginOperation([]() { std::wstring message; const bool ok = RestartInternal(message); PostDone(ok, 0, message); }); break;
    case 2005: OpenWeb(); break;
    case 2006: g_shuttingDown = true; DestroyWindow(hwnd); break;
    default: break;
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_mainWindow = hwnd;
        CreateUi(hwnd);
        AddTrayIcon(hwnd);
        return 0;
    case WM_SIZE:
        LayoutUi(hwnd);
        if (wParam == SIZE_MINIMIZED) ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_TIMER:
        if (wParam == kTimerStatus) RefreshStatusInBackground();
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int notification = HIWORD(wParam);
        if ((id == IDC_DSH_PATH || id == IDC_NPM_PATH) && notification == EN_KILLFOCUS) ApplyPathsFromUi();
        if (id == IDC_START) { ApplyPathsFromUi(); BeginOperation([]() { std::wstring message; const bool ok = StartInternal(message); PostDone(ok, 0, message); }); }
        else if (id == IDC_STOP) BeginOperation([]() { std::wstring message; const bool ok = StopInternal(message); PostDone(ok, 0, message); });
        else if (id == IDC_RESTART) { ApplyPathsFromUi(); BeginOperation([]() { std::wstring message; const bool ok = RestartInternal(message); PostDone(ok, 0, message); }); }
        else if (id == IDC_OPEN) OpenWeb();
        else if (id == IDC_DETECT) DetectPaths(false);
        else if (id == IDC_DSH_BROWSE) BrowseCommand(g_dshPathEdit, L"选择 dsh.cmd");
        else if (id == IDC_NPM_BROWSE) BrowseCommand(g_npmPathEdit, L"选择 npm.cmd");
        else if (id == IDC_REFRESH) RefreshVersions();
        else if (id == IDC_SWITCH) SwitchVersion();
        else if (id == IDC_RUN_LOGIN && notification == BN_CLICKED) SetRunAtLogin(SendMessageW(g_loginCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        if (control == g_statusLabel) {
            // The status text is right-aligned and its control grows when the
            // window is widened. Paint an opaque background so the previous
            // right-aligned text cannot remain as a resize artifact.
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, RGB(250, 250, 250));
            const std::wstring text = [&]() { wchar_t buffer[128]{}; GetWindowTextW(control, buffer, std::size(buffer)); return std::wstring(buffer); }();
            SetTextColor(dc, text.find(L"运行中") != std::wstring::npos ? RGB(0, 128, 0) : text.find(L"启动中") != std::wstring::npos ? RGB(190, 110, 0) : RGB(100, 100, 100));
        } else {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(45, 45, 45));
        }
        return reinterpret_cast<LRESULT>(g_windowBrush);
    }
    case kTrayMessage:
        if (lParam == WM_LBUTTONDBLCLK) { ShowWindow(hwnd, SW_SHOW); ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
        else if (lParam == WM_RBUTTONUP) ShowTrayMenu(hwnd);
        return 0;
    case kAppLog: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
        if (text) AppendLogToEdit(*text);
        return 0;
    }
    case kAppStatus: {
        std::unique_ptr<StatusMessage> status(reinterpret_cast<StatusMessage*>(lParam));
        g_statusBusy = false;
        if (status) {
            UpdateStatusLabel(status->state);
            if (!status->version.empty()) WriteTextControl(g_currentVersion, status->version);
            else if (g_dshPath.empty()) WriteTextControl(g_currentVersion, L"未检测到 dsh.cmd");
            else WriteTextControl(g_currentVersion, L"无法读取");
        }
        return 0;
    }
    case kAppDone: {
        std::unique_ptr<DoneMessage> done(reinterpret_cast<DoneMessage*>(lParam));
        g_operationBusy = false;
        EnableWindow(g_detectButton, TRUE);
        EnableWindow(g_refreshButton, TRUE);
        EnableWindow(g_switchButton, TRUE);
        EnableWindow(g_dshPathEdit, TRUE);
        EnableWindow(g_npmPathEdit, TRUE);
        if (done) {
            if (done->action == 10 || done->action == 11) {
                WriteTextControl(g_dshPathEdit, g_dshPath);
                WriteTextControl(g_npmPathEdit, g_npmPath);
                SaveSettings();
            }
            if (!done->text.empty()) Log(done->text);
            if (!done->success) MessageBoxW(hwnd, done->text.c_str(), L"操作失败", MB_OK | MB_ICONWARNING);
            if (done->action == 10) RefreshVersions();
        }
        RefreshStatusInBackground();
        return 0;
    }
    case kAppVersions: {
        std::unique_ptr<VersionsMessage> versions(reinterpret_cast<VersionsMessage*>(lParam));
        if (versions) {
            g_versions = std::move(versions->versions);
            SendMessageW(g_versionCombo, CB_RESETCONTENT, 0, 0);
            for (const auto& version : g_versions) SendMessageW(g_versionCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(version.c_str()));
            if (!g_versions.empty()) SendMessageW(g_versionCombo, CB_SETCURSEL, 0, 0);
            WriteTextControl(g_latestLabel, L"最新稳定版：" + (versions->latestStable.empty() ? L"暂无（当前可能只有预发布版）" : versions->latestStable));
            Log(L"npm 版本检查完成：共 " + std::to_wstring(g_versions.size()) + L" 个可安装版本。 ");
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_QUERYENDSESSION:
        g_shuttingDown = true;
        return TRUE;
    case WM_DESTROY:
        KillTimer(hwnd, kTimerStatus);
        RemoveTrayIcon(hwnd);
        g_shuttingDown = true;
        {
            std::wstring message;
            StopInternal(message);
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX commonControls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&commonControls);

    g_instanceMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!g_instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(nullptr, kWindowTitle);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        if (g_instanceMutex) CloseHandle(g_instanceMutex);
        return 0;
    }

    g_logPath = LogDirectory() + L"\\dsh-latest.log";
    g_windowBrush = CreateSolidBrush(RGB(250, 250, 250));
    g_font = CreateFontW(-MulDiv(9, GetDeviceCaps(GetDC(nullptr), LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_boldFont = CreateFontW(-MulDiv(9, GetDeviceCaps(GetDC(nullptr), LOGPIXELSY), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_titleFont = CreateFontW(-MulDiv(17, GetDeviceCaps(GetDC(nullptr), LOGPIXELSY), 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = L"DshStartControlNativeWindow";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = g_windowBrush;
    RegisterClassExW(&windowClass);

    g_mainWindow = CreateWindowExW(WS_EX_APPWINDOW, windowClass.lpszClassName, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 720, nullptr, nullptr, instance, nullptr);
    if (!g_mainWindow) return 1;
    ShowWindow(g_mainWindow, showCommand == SW_HIDE ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(g_mainWindow);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (g_font) DeleteObject(g_font);
    if (g_boldFont) DeleteObject(g_boldFont);
    if (g_titleFont) DeleteObject(g_titleFont);
    if (g_windowBrush) DeleteObject(g_windowBrush);
    if (g_instanceMutex) CloseHandle(g_instanceMutex);
    return static_cast<int>(message.wParam);
}
