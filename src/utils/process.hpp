#pragma once

#include <string>
#include <vector>

namespace utils
{
// Execute argv directly: configuration values are never interpreted by a shell.
int RunCommand(const std::vector<std::string> &arguments);
bool IsInterfaceName(const std::string &name);
} // namespace utils
