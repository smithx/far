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
std::thread                  Thread;

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

    spdlog::set_default_logger(spdlog::basic_logger_mt("plugin", path + "\\gitbranch.log"));
    spdlog::flush_on(spdlog::level::debug);
    spdlog::set_pattern("%d %b %H:%M:%S.%e [%-5P:%5t] [%l] %v");
    spdlog::set_level(spdlog::level::debug);

    spdlog::info("SetStartupInfoW: enter");

    PSI = *psi;
    FSF = *psi->FSF;
    PSI.FSF = &FSF;
    Heap = GetProcessHeap();

    SetEnvironmentVariable(EnvVar, L"");

    Thread = std::thread(Run);

    spdlog::info("SetStartupInfoW: exit");
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
    spdlog::info("ExitFARW: enter, [Running: {}]", Running.load());

    Running = false;
    PauseVariable.notify_one();

    if (Thread.joinable()) {
        Thread.join();
    }

    spdlog::info("ExitFARW: exit, Plugin thread joined, [Running: {}]", Running.load());
}

void Run()
{
    spdlog::info("Plugin thread stated, [Running: {}]", Running.load());
    while(Running)
    {
        PSI.AdvControl(&MainGuid, ACTL_SYNCHRO, 0, nullptr);

        std::unique_lock<std::mutex> lock(PauseMutex);
        PauseVariable.wait_for(lock, SynchroFarRequestTimeout, []{ return !Running; });
    }
    spdlog::info("Plugin thread exit, [Running: {}]", Running.load());
}


std::string GetGitBranchName(std::wstring);
std::wstring GetEnvVar();
bool Timeout();

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

    if (PreviousDir != directory || Timeout()) {
        std::string branch;

        if (directory.size()) {
            branch = GetGitBranchName(directory);
            if (branch.size()) {
                branch = " (" + branch + ")";
            }
        }

        std::wstring wbranch = std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().from_bytes(branch);

        PreviousDir = directory;
        PreviousUpdateTimePoint = std::chrono::steady_clock::now();

        if (GetEnvVar() != wbranch) {
            SetEnvironmentVariable(EnvVar, wbranch.c_str());
            PSI.AdvControl(&MainGuid, ACTL_REDRAWALL, 0, nullptr);
        }
    }

    return 0;
}

bool Timeout()
{
    return std::chrono::steady_clock::now() - PreviousUpdateTimePoint > ForceUpdateTimeout;
}

std::wstring GetEnvVar()
{
    wchar_t buf[1024];
    return { buf, GetEnvironmentVariable(EnvVar, buf, 1024) };
}


std::string selectCurrentBrunch(std::string&& out);
std::string prettifyDetached(std::string&& name);

std::string GetGitBranchName(std::wstring dir)
{
    HANDLE stdoutRd = INVALID_HANDLE_VALUE;
    HANDLE stdoutWr = INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES  attributes;
    attributes.nLength              = sizeof(SECURITY_ATTRIBUTES);
    attributes.bInheritHandle       = TRUE;
    attributes.lpSecurityDescriptor = nullptr;

    if (!CreatePipe(&stdoutRd, &stdoutWr, &attributes, 0) || !SetHandleInformation(stdoutRd, HANDLE_FLAG_INHERIT, 0)) {
        return "";
    }

    PROCESS_INFORMATION  processInfo = {};
    STARTUPINFO          startupInfo = {};

    startupInfo.cb         = sizeof(STARTUPINFO);
    startupInfo.hStdInput  = 0;
    startupInfo.hStdOutput = stdoutWr;
    startupInfo.hStdError  = 0;

    if (stdoutRd)
        startupInfo.dwFlags |= STARTF_USESTDHANDLES;

    // Make a copy because CreateProcess needs to modify string buffer
    wchar_t cmd[MAX_PATH];
    wcsncpy(cmd, GitCmd, MAX_PATH);
    cmd[MAX_PATH - 1] = 0;

    int created = CreateProcess(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &startupInfo, &processInfo);
    CloseHandle(stdoutWr);

    if (created) {
        CloseHandle(processInfo.hThread);
    }
    else {
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
        CloseHandle(stdoutRd);
        return "";
    }

    std::string output;
    std::thread rdThread;

    if (stdoutRd) {
        rdThread = std::thread([&]() {
            const size_t bufsize = 1024;
            char         buffer[bufsize];
            for (;;) {
                DWORD n = 0;
                int read = ReadFile(stdoutRd, buffer, (DWORD)bufsize, &n, nullptr);
                if (!read || n == 0)
                    break;
                output += std::string{ buffer, n };
            }
        });
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);

    CloseHandle(processInfo.hProcess);

    if (rdThread.joinable())
        rdThread.join();

    CloseHandle(stdoutRd);

    return selectCurrentBrunch(std::move(output));
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