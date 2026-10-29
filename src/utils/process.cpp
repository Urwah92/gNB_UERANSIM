#include "process.hpp"

#include <cerrno>
#include <cctype>
#include <net/if.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

namespace utils
{
CommandResult RunCommandDetailed(const std::vector<std::string> &arguments)
{
    if (arguments.empty() || arguments.front().empty())
        return {EINVAL, 0, 0};
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
    {
        if (argument.find('\0') != std::string::npos)
            return {EINVAL, 0, 0};
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid{};
    int result = posix_spawnp(&pid, argv.front(), nullptr, nullptr, argv.data(), environ);
    if (result != 0)
        return {result, 0, 0};
    int status{};
    while (waitpid(pid, &status, 0) == -1)
    {
        if (errno != EINTR)
            return {errno, 0, 0};
    }
    if (WIFEXITED(status))
        return {0, WEXITSTATUS(status), 0};
    return {0, 0, WTERMSIG(status)};
}

int RunCommand(const std::vector<std::string> &arguments)
{
    const auto result = RunCommandDetailed(arguments);
    if (result.spawnError)
        return result.spawnError;
    return result.signal ? 128 + result.signal : result.exitCode;
}

bool IsInterfaceName(const std::string &name)
{
    if (name.empty() || name == "." || name == ".." || name.size() >= IFNAMSIZ || name.front() == '-')
        return false;
    for (unsigned char character : name)
        if (!std::isalnum(character) && character != '_' && character != '-' && character != '.')
            return false;
    return true;
}
} // namespace utils
