//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include "base_app.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <vector>

static std::atomic_int g_instanceCount{};
static std::vector<void (*)()> g_runAtExit{};
static std::vector<std::string> g_deleteAtExit{};
static volatile std::sig_atomic_t g_shutdownRequested{};
static std::atomic_bool g_taskShutdownRequested{};

extern "C" void BaseSignalHandler(int num)
{
    if (num == SIGTERM || num == SIGINT)
        g_shutdownRequested = 1;
}

namespace app
{

void Initialize()
{
    if (g_instanceCount++ != 0)
        std::terminate();

    srand(time(nullptr));

    std::signal(SIGTERM, BaseSignalHandler);
    std::signal(SIGINT, BaseSignalHandler);
}

void RunAtExit(void (*fun)())
{
    g_runAtExit.push_back(fun);
}

void RequestShutdown()
{
    g_taskShutdownRequested.store(true);
}

bool ShutdownRequested()
{
    return g_shutdownRequested != 0 || g_taskShutdownRequested.load();
}

void Finalize()
{
    for (auto fun : g_runAtExit)
        fun();
    for (const auto &file : g_deleteAtExit)
        std::remove(file.c_str());
    g_runAtExit.clear();
    g_deleteAtExit.clear();
}

void DeleteAtExit(const std::string &file)
{
    g_deleteAtExit.push_back(file);
}

} // namespace app
