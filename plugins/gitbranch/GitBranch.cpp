#include <plugin.hpp>
#include <initguid.h>
#include "version.hpp"
#include "guid.hpp"
#include <string>
#include <thread>
#include <chrono>

#include <mutex>
#include <atomic>
#include <condition_variable>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

std::shared_ptr<spdlog::logger> file_logger;

const wchar_t*               EnvVar = L"GITBRANCH";
const wchar_t*               GitCmd = L"git branch";
std::atomic<bool>            Running = true;

PluginStartupInfo            PSI;
FarStandardFunctions         FSF;
HANDLE                       Heap;

std::mutex                   PauseMutex;
std::condition_variable      PauseVariable;
std::unique_ptr<std::thread> Thread;

void WINAPI GetGlobalInfoW(struct GlobalInfo *Info)
{
    Info->StructSize=sizeof(struct GlobalInfo);
    Info->MinFarVersion=FARMANAGERVERSION;
    Info->Version=PLUGIN_VERSION;
    Info->Guid=MainGuid;
    Info->Title=PLUGIN_NAME;
    Info->Description=PLUGIN_DESC;
    Info->Author=PLUGIN_AUTHOR;
}

void SetupEnvVar();

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

    Thread = std::make_unique<std::thread>(SetupEnvVar);
    
    spdlog::info("SetStartupInfoW exit");
}

void WINAPI GetPluginInfoW(struct PluginInfo *Info)
{
    Info->StructSize = sizeof(*Info);
    Info->Flags = PF_PRELOAD;
}

HANDLE WINAPI OpenW(const struct OpenInfo *OInfo)
{
    return NULL;
}

void WINAPI ExitFARW(const ExitInfo* Info)
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

std::string GetGitBranchName(std::wstring);

void SetupEnvVar()
{
    spdlog::info("SetupEnvVar thread stated, running: {}", Running.load());
    std::wstring prev_dir_name;
    while(Running)
    {
        //Current directory
        std::wstring dir_name;
        const size_t pd_length = static_cast<size_t>(PSI.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, 0, NULL));
        if (pd_length) {
            FarPanelDirectory* pd = static_cast<FarPanelDirectory*>(HeapAlloc(Heap, HEAP_ZERO_MEMORY, pd_length));
            if (pd) {
                pd->StructSize = sizeof(FarPanelDirectory);
                if (PSI.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, static_cast<int>(pd_length), pd)) {
                    dir_name = pd->Name;
                }
                HeapFree(Heap, 0, pd);
            }
        }

        if (prev_dir_name != dir_name)
        {
            std::string branch = GetGitBranchName(dir_name);
            if (branch.size())
            {
                branch = " (" + branch + ")";
            }

            SetEnvironmentVariable(EnvVar, std::wstring(branch.begin(), branch.end()).c_str()); // XXX
            prev_dir_name = dir_name;
            PSI.AdvControl(&MainGuid, ACTL_REDRAWALL, 0, nullptr);
        }

        std::unique_lock<std::mutex> lock(PauseMutex);
        PauseVariable.wait_for(lock, std::chrono::milliseconds(333), []{ return !Running; });
    }
    spdlog::info("SetupEnvVar thread exit, running: {}", Running.load());
}

std::string selectCurrentBrunch(std::string&& out);
std::string prettifyDetached(std::string&& name);

std::string GetGitBranchName(std::wstring CmdRunDir)
{
    int                  Success;
    SECURITY_ATTRIBUTES  security_attributes;
    HANDLE               stdout_rd = INVALID_HANDLE_VALUE;
    HANDLE               stdout_wr = INVALID_HANDLE_VALUE;
    HANDLE               stderr_rd = INVALID_HANDLE_VALUE;
    HANDLE               stderr_wr = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION  process_info;
    STARTUPINFO          startup_info;
    std::thread          stdout_thread;
    std::thread          stderr_thread;

    security_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    security_attributes.bInheritHandle = TRUE;
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

    startup_info.cb = sizeof(STARTUPINFO);
    startup_info.hStdInput = 0;
    startup_info.hStdOutput = stdout_wr;
    startup_info.hStdError = stderr_wr;

    if (stdout_rd || stderr_rd)
        startup_info.dwFlags |= STARTF_USESTDHANDLES;

    // Make a copy because CreateProcess needs to modify string buffer
    wchar_t  CmdLineStr[MAX_PATH];
    wcsncpy(CmdLineStr, GitCmd, MAX_PATH);
    CmdLineStr[MAX_PATH - 1] = 0;

    Success = CreateProcess(nullptr, CmdLineStr, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, CmdRunDir.c_str(), &startup_info, &process_info);
    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    if (Success) {
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
                int Success = ReadFile(stdout_rd, buffer, (DWORD)bufsize, &n, nullptr);
                if (!Success || n == 0)
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
    while (end != std::string::npos)
    {
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
    const char* detached_prefix = "(HEAD detached at ";
    static const size_t detached_len = strlen(detached_prefix);
    if (name.substr(0, detached_len) == detached_prefix) {
        return name.substr(detached_len, name.size() - detached_len - 1) + "...";
    }
    return name;
}