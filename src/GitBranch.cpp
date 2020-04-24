#include <plugin.hpp>
#include <initguid.h>
#include "version.hpp"
#include "guid.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <chrono>
#include <locale>
#include <codecvt>
#include <condition_variable>

std::shared_ptr<spdlog::logger> file_logger;

const wchar_t*               EnvVar = L"GITBRANCH";
const wchar_t*               GitCmd = L"git branch";

std::chrono::milliseconds    SynchroFarRequestTimeout{ 333 };
std::chrono::seconds         ForceUpdateTimeout{ 5 };

std::atomic<bool>            Running = true;

PluginStartupInfo            PSI;
FarStandardFunctions         FSF;
HANDLE                       Heap;

std::mutex                   PauseMutex;
std::condition_variable      PauseVariable;
std::unique_ptr<std::thread> Thread;

std::wstring                 PreviousDir;
std::chrono::time_point<std::chrono::steady_clock> PreviousUpdateTimePoint = std::chrono::steady_clock::now();

void WINAPI GetGlobalInfoW(struct GlobalInfo *Info)
{
    Info->StructSize=sizeof(struct GlobalInfo);
    Info->MinFarVersion= MAKEFARVERSION(FARMANAGERVERSION_MAJOR, FARMANAGERVERSION_MINOR, FARMANAGERVERSION_REVISION, 5555, VS_RELEASE);
    Info->Version=PLUGIN_VERSION;
    Info->Guid=MainGuid;
    Info->Title=PLUGIN_NAME;
    Info->Description=PLUGIN_DESC;
    Info->Author=PLUGIN_AUTHOR;
}

void Run();

void WINAPI SetStartupInfoW(const PluginStartupInfo *psi)
{
    char buf[MAX_PATH];
    size_t len = GetTempPathA(MAX_PATH, buf);
    std::string path { buf };
    file_logger = spdlog::basic_logger_mt("plugin", path + "\\gitbranch.log");

    spdlog::set_default_logger(file_logger);
    spdlog::set_level(spdlog::level::debug);

    spdlog::info("SetStartupInfoW start");

    PSI = *psi;
    FSF = *psi->FSF;
    PSI.FSF = &FSF;
    Heap = GetProcessHeap();

    SetEnvironmentVariable(EnvVar, L"");

    Thread = std::make_unique<std::thread>(Run);

    spdlog::info("SetStartupInfoW exit");
}

void WINAPI GetPluginInfoW(struct PluginInfo *Info)
{
    Info->StructSize = sizeof(*Info);
    Info->Flags = PF_PRELOAD;
}

HANDLE WINAPI OpenW(const struct OpenInfo*)
{
    return NULL;
}

void WINAPI ExitFARW(const ExitInfo*)
{
    spdlog::info("ExitFARW  running: {}", Running.load());

    Running = false;
    PauseVariable.notify_one();

    if (Thread && Thread->joinable()) {
        Thread->join();
    }

    Thread.reset();

    spdlog::info("ExitFARW thread joined, running: {}", Running.load());
}

void Run()
{
    spdlog::info("Plugin thread stated, running: {}", Running.load());
    while(Running)
    {
        PSI.AdvControl(&MainGuid, ACTL_SYNCHRO, 0, nullptr);

        std::unique_lock<std::mutex> lock(PauseMutex);
        PauseVariable.wait_for(lock, SynchroFarRequestTimeout, []{ return !Running; });
    }
    spdlog::info("Plugin thread exit, running: {}", Running.load());
}


std::string GetGitBranchName(std::wstring);
bool timeout();

intptr_t WINAPI ProcessSynchroEventW(const struct ProcessSynchroEventInfo*)
{
    //Get current directory
    std::wstring directory;
    if (const size_t length = static_cast<size_t>(PSI.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, 0, NULL))) {
        if (FarPanelDirectory* pd = static_cast<FarPanelDirectory*>(HeapAlloc(Heap, HEAP_ZERO_MEMORY, length))) {
            pd->StructSize = sizeof(FarPanelDirectory);
            if (PSI.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, static_cast<int>(length), pd)) {
                directory = pd->Name;
            }
            HeapFree(Heap, 0, pd);
        }
    }

    if (PreviousDir != directory || timeout()) {
        std::string branch;

        if (directory.size()) {
            branch = GetGitBranchName(directory);
            if (branch.size()) {
                branch = " (" + branch + ")";
            }
        }

        std::wstring wbranch = std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().from_bytes(branch);

        SetEnvironmentVariable(EnvVar, wbranch.c_str());
        PreviousDir = directory;
        PreviousUpdateTimePoint = std::chrono::steady_clock::now();
        PSI.AdvControl(&MainGuid, ACTL_REDRAWALL, 0, nullptr);
    }

    return 0;
}

bool timeout()
{
    return std::chrono::steady_clock::now() - PreviousUpdateTimePoint > ForceUpdateTimeout;
}


std::string selectCurrentBrunch(std::string&& out);
std::string prettifyDetached(std::string&& name);

std::string GetGitBranchName(std::wstring dir)
{
    SECURITY_ATTRIBUTES  security_attributes;
    HANDLE               stdout_rd = INVALID_HANDLE_VALUE;
    HANDLE               stdout_wr = INVALID_HANDLE_VALUE;
    HANDLE               stderr_rd = INVALID_HANDLE_VALUE;
    HANDLE               stderr_wr = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION  process_info;
    STARTUPINFO          startup_info;
    std::thread          stdout_thread;
    std::thread          stderr_thread;

    security_attributes.nLength              = sizeof(SECURITY_ATTRIBUTES);
    security_attributes.bInheritHandle       = TRUE;
    security_attributes.lpSecurityDescriptor = nullptr;

    if (!CreatePipe(&stdout_rd, &stdout_wr, &security_attributes, 0) || !SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0)) {
        return "";
    }

    if (!CreatePipe(&stderr_rd, &stderr_wr, &security_attributes, 0) || !SetHandleInformation(stderr_rd, HANDLE_FLAG_INHERIT, 0)) {
        if (stdout_rd != INVALID_HANDLE_VALUE) CloseHandle(stdout_rd);
        if (stdout_wr != INVALID_HANDLE_VALUE) CloseHandle(stdout_wr);
        return "";
    }

    ZeroMemory(&process_info, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&startup_info, sizeof(STARTUPINFO));

    startup_info.cb         = sizeof(STARTUPINFO);
    startup_info.hStdInput  = 0;
    startup_info.hStdOutput = stdout_wr;
    startup_info.hStdError  = stderr_wr;

    if (stdout_rd || stderr_rd)
        startup_info.dwFlags |= STARTF_USESTDHANDLES;

    // Make a copy because CreateProcess needs to modify string buffer
    wchar_t cmd[MAX_PATH];
    wcsncpy(cmd, GitCmd, MAX_PATH);
    cmd[MAX_PATH - 1] = 0;

    int created = CreateProcess(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &startup_info, &process_info);
    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    if (created) {
        CloseHandle(process_info.hThread);
    }
    else {
        CloseHandle(process_info.hProcess);
        CloseHandle(process_info.hThread);
        CloseHandle(stdout_rd);
        CloseHandle(stderr_rd);
        return "";
    }

    std::string out;

    if (stdout_rd) {
        stdout_thread = std::thread([&]() {
            const size_t bufsize = 1024;
            char         buffer[bufsize];
            for (;;) {
                DWORD n = 0;
                int read = ReadFile(stdout_rd, buffer, (DWORD)bufsize, &n, nullptr);
                if (!read || n == 0)
                    break;
                out += std::string{ buffer, n };
            }
        });
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);

    CloseHandle(process_info.hProcess);

    if (stdout_thread.joinable())
        stdout_thread.join();

    if (stderr_thread.joinable())
        stderr_thread.join();

    CloseHandle(stdout_rd);
    CloseHandle(stderr_rd);

    return selectCurrentBrunch(std::move(out));
}

std::string selectCurrentBrunch(std::string&& out)
{
    size_t begin = 0;
    size_t end = out.find_first_of('\n');
    while (end != std::string::npos) {
        if (out[begin] == '*') {
            out = out.substr(begin, end - begin);
            return prettifyDetached(out.substr(2));
        }

        begin = end + 1;
        end = out.find_first_of('\n', begin);
    }

    return "";
}

std::string prettifyDetached(std::string&& name)
{
    const char* prefix = "(HEAD detached at ";
    static const size_t length = strlen(prefix);
    if (name.substr(0, length) == prefix) {
        return name.substr(length, name.size() - length - 1) + "...";
    }

    return name;
}