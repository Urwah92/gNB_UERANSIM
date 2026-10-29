#pragma once

#include <string>
#include <vector>

namespace utils
{
struct CommandResult
{
    // errno from argument validation, posix_spawnp, or waitpid; zero when child status is available.
    int spawnError{};
    int exitCode{};
    int signal{};
};

// Execute argv directly and preserve inherited stdout/stderr. Values are never interpreted by a shell.
CommandResult RunCommandDetailed(const std::vector<std::string> &arguments);
// Legacy status: errno on launch/wait failure, otherwise exit code or 128 + terminating signal.
int RunCommand(const std::vector<std::string> &arguments);
bool IsInterfaceName(const std::string &name);
} // namespace utils
