#include <utils/process.hpp>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void Require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

const std::vector<std::string> Arguments{
    "space value", "quotes'\"", ";$(literal)`text`|&<>*?", "line\nbreak", "", "-option"};

void Commands(const std::string &self)
{
    std::vector<std::string> command{self, "--argument-child"};
    command.insert(command.end(), Arguments.begin(), Arguments.end());
    Require(utils::RunCommand(command) == 0, "Command arguments changed during spawn");
    Require(utils::RunCommand({self, "--exit-child", "0"}) == 0, "Success status changed");
    Require(utils::RunCommand({self, "--exit-child", "37"}) == 37, "Failure status changed");
    Require(utils::RunCommand({self, "--signal-child"}) == 128 + SIGTERM, "Signal status changed");
    Require(utils::RunCommand({}) == EINVAL, "Empty command accepted");
    Require(utils::RunCommand({"/a-nonexistent-ueransim-test-executable"}) == ENOENT,
            "Missing executable did not report ENOENT");
    Require(utils::RunCommand({""}) == EINVAL, "Empty executable accepted");
    Require(utils::RunCommand({self, std::string("x\0y", 3)}) == EINVAL, "Embedded NUL argument silently truncated");
    Require(utils::RunCommand({std::string("x\0y", 3)}) == EINVAL, "Embedded NUL executable silently truncated");
}

void Interfaces()
{
    for (const auto &name : {"eth0", "enp0s3", "wlan-1", "br.test", "a_1", "123456789012345"})
        Require(utils::IsInterfaceName(name), "Valid interface name rejected");
    for (const auto &name : {"", "-eth0", "1234567890123456", "a b", "eth0;", "a/b", "a:b", ".", "..", "a\nb"})
        Require(!utils::IsInterfaceName(name), "Invalid interface name accepted");
    Require(!utils::IsInterfaceName(std::string("a\0b", 3)), "NUL in interface name accepted");
}
} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && std::string(argv[1]) == "--argument-child")
    {
        if (argc != static_cast<int>(Arguments.size()) + 2)
            return 1;
        for (size_t i = 0; i < Arguments.size(); ++i)
            if (argv[i + 2] != Arguments[i])
                return 2;
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--exit-child")
        return std::stoi(argv[2]);
    if (argc == 2 && std::string(argv[1]) == "--signal-child")
    {
        std::raise(SIGTERM);
        return 1;
    }
    try
    {
        Commands(argv[0]);
        Interfaces();
        std::cout << "Process tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
