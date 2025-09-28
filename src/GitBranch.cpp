#include <plugin.hpp>
#include <initguid.h>
#include "version.hpp"
#include "guid.hpp"

#include <git2/errors.h>
#include <git2/global.h>
#include <git2/refs.h>
#include <git2/repository.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>


#include <filesystem>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <vector>

constexpr char               EnvVar[] = "GITBRANCH";

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

    SetEnvironmentVariableA(EnvVar, "");

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


std::string GetGitBranchName(std::filesystem::path);
std::string GetEnvVar();
bool Timeout();

intptr_t WINAPI ProcessSynchroEventW(const struct ProcessSynchroEventInfo*)
{
    //Get current directory
    std::wstring directory;
    if (const size_t length = static_cast<size_t>(PSI.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, 0, NULL))) {
        if (auto pd = static_cast<FarPanelDirectory*>(HeapAlloc(Heap, HEAP_ZERO_MEMORY, length))) {
            pd->StructSize = sizeof(FarPanelDirectory);
            if (PSI.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, static_cast<int>(length), pd)) {
                directory = pd->Name;
            }
            HeapFree(Heap, 0, pd);
        }
    }

    if (PreviousDir != directory || Timeout()) {
        std::string branch;

        if (!directory.empty()) {
            branch = GetGitBranchName(directory);
            if (!branch.empty()) {
                branch = " (" + branch + ")";
            }
        }

        PreviousDir = directory;
        PreviousUpdateTimePoint = std::chrono::steady_clock::now();

        if (GetEnvVar() != branch) {
            SetEnvironmentVariableA(EnvVar, branch.c_str());
            PSI.AdvControl(&MainGuid, ACTL_REDRAWALL, 0, nullptr);
        }
    }

    return 0;
}

bool Timeout()
{
    return std::chrono::steady_clock::now() - PreviousUpdateTimePoint > ForceUpdateTimeout;
}

std::string GetEnvVar()
{
    char buf[1024];
    return { buf, GetEnvironmentVariableA(EnvVar, buf, 1024) };
}

struct GitRepository
{
    ~GitRepository() { git_repository_free(repo); }
    operator git_repository* () const { return repo; }
    git_repository** operator&() { return &repo; }
    git_repository* repo = nullptr;
};

struct GitReference
{
    operator git_reference* () const { return ref; }
    git_reference** operator&() { return &ref; }
    ~GitReference() { if (ref != nullptr) { git_reference_free(ref); } }
    git_reference* ref = nullptr;
};

struct GitInit
{
    GitInit() { git_libgit2_init(); }
    ~GitInit() { git_libgit2_shutdown(); }
};

std::string GetGitBranchName(std::filesystem::path dir)
{
    GitInit git;
    git_buf out{};
    if (git_repository_discover(&out, dir.u8string().c_str(), 0, nullptr) != 0)
        return "";

    GitRepository repo;
    if (git_repository_open(&repo, out.ptr) != 0)
        return "";

    GitReference head;
    switch (git_repository_head(&head, repo))
    {
    case 0: //ok
        if (git_repository_head_detached(repo))
            return std::string{ git_oid_tostr_s(git_reference_target(head)) }.substr(0, 9) + "...";
        else
            return git_reference_shorthand(head);
    case GIT_EUNBORNBRANCH: // non-existing branch
    case GIT_ENOTFOUND: // HEAD is missing
        return "HEAD (no branch)";
    default: // error
        return "";
    }
}
