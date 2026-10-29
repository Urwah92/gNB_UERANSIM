#include "process.hpp"

#include <cerrno>
#include <cctype>
#include <net/if.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

namespace utils
{
int RunCommand(const std::vector<std::string> &arguments)
{
    if (arguments.empty() || arguments.front().empty())
        return EINVAL;
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
    {
        if (argument.find('\0') != std::string::npos)
            return EINVAL;
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid{};
    int result = posix_spawnp(&pid, argv.front(), nullptr, nullptr, argv.data(), environ);
    if (result != 0)
        return result;
    int status{};
    while (waitpid(pid, &status, 0) == -1)
    {
        if (errno != EINTR)
            return errno;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
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
