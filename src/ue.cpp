//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <lib/app/base_app.hpp>
#include <lib/app/cli_base.hpp>
#include <lib/app/cli_cmd.hpp>
#include <lib/app/proc_table.hpp>
#include <lib/app/ue_ctl.hpp>
#include <ue/ue.hpp>
#include <utils/common.hpp>
#include <utils/concurrent_map.hpp>
#include <utils/constants.hpp>
#include <utils/options.hpp>
#include <utils/yaml_utils.hpp>
#include <utils/process.hpp>
#include <yaml-cpp/yaml.h>

static app::CliServer *g_cliServer = nullptr;
static nr::ue::UeConfig *g_refConfig = nullptr;
static ConcurrentMap<std::string, nr::ue::UserEquipment *> g_ueMap{};
static app::CliResponseTask *g_cliRespTask = nullptr;

static struct Options
{
    std::string configFile{};
    bool noRoutingConfigs{};
    bool disableCmd{};
    bool checkConfig{};
    std::string imsi{};
    int count{};
    int tempo{};
} g_options{};

struct NwUeControllerCmd : NtsMessage
{
    enum PR
    {
        PERFORM_SWITCH_OFF,
    } present;

    // PERFORM_SWITCH_OFF
    nr::ue::UserEquipment *ue{};

    explicit NwUeControllerCmd(PR present) : NtsMessage(NtsMessageType::UE_CTL_COMMAND), present(present)
    {
    }
};

class UeControllerTask : public NtsTask
{
  protected:
    void onStart() override
    {
    }

    void onLoop() override
    {
        auto msg = take();
        if (msg == nullptr)
            return;
        if (msg->msgType == NtsMessageType::UE_CTL_COMMAND)
        {
            auto &w = dynamic_cast<NwUeControllerCmd &>(*msg);
            switch (w.present)
            {
            case NwUeControllerCmd::PERFORM_SWITCH_OFF: {
                std::string key{};
                g_ueMap.invokeForeach([&key, &w](auto &item) {
                    if (item.second == w.ue)
                        key = item.first;
                });

                if (key.empty())
                    return;

                const bool lastUe = g_ueMap.removeAndGetSize(key) == 0;
                delete w.ue;
                if (lastUe)
                    app::RequestShutdown();
                break;
            }
            }
        }
    }

    void onQuit() override
    {
    }
};

static UeControllerTask *g_controllerTask;

static std::string ReadInterface(const YAML::Node &config, const std::string &key)
{
    auto name = yaml::GetString(config, key, 1, IFNAMSIZ - 1);
    if (!utils::IsInterfaceName(name))
        throw std::runtime_error("Invalid network interface name in field '" + key + "'");
    return name;
}

static void CheckWifiNetwork(const nr::ue::UeConfig &config)
{
    if (!config.wifi)
        return;

    const std::string context = "Wi-Fi startup: staticIP '" + config.staticIP + "' on Interface '" + config.interface + "' ";
    const std::string guidance = ". Check 'ip -br -4 addr' in this network namespace and configure staticIP on "
                                 "the intended Interface before starting the UE";
    ifaddrs *raw = nullptr;
    if (getifaddrs(&raw) != 0)
        throw std::runtime_error(context + "could not inspect local interfaces: " + std::strerror(errno) + guidance);
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> addresses(raw, freeifaddrs);
    in_addr requested{};
    inet_pton(AF_INET, config.staticIP.c_str(), &requested);
    bool exists = false;
    bool up = false;
    bool assigned = false;
    for (auto *entry = raw; entry; entry = entry->ifa_next)
    {
        if (!entry->ifa_name || config.interface != entry->ifa_name)
            continue;
        exists = true;
        up = up || (entry->ifa_flags & IFF_UP);
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET)
            continue;
        const auto address = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr)->sin_addr.s_addr;
        assigned = assigned || address == requested.s_addr;
        // Linux permits loopback aliases inside its assigned loopback subnet without listing
        // each address separately (e.g. 127.0.0.6 with 127.0.0.1/8). Confirm binding below.
        if ((entry->ifa_flags & IFF_LOOPBACK) && (ntohl(requested.s_addr) >> 24) == 127 && entry->ifa_netmask)
        {
            const auto mask = reinterpret_cast<const sockaddr_in *>(entry->ifa_netmask)->sin_addr.s_addr;
            assigned = assigned || (address & mask) == (requested.s_addr & mask);
        }
    }
    if (!exists)
        throw std::runtime_error(context + "does not exist" + guidance);
    if (!up)
        throw std::runtime_error(context + "is not up" + guidance);
    if (!assigned)
        throw std::runtime_error(context + "is not assigned to that interface" + guidance);

    // Probe before starting any tasks: interface inspection alone cannot guarantee bindability.
    Socket probe;
    try
    {
        probe = Socket::CreateUdp4();
        probe.bind(InetAddress(config.staticIP, 0));
        probe.close();
    }
    catch (const std::exception &error)
    {
        probe.close();
        throw std::runtime_error(context + "cannot bind a local UDP socket: " + error.what() + guidance);
    }
}

static nr::ue::UeConfig *ReadConfigYaml()
{
    auto *result = new nr::ue::UeConfig();
    auto config = YAML::LoadFile(g_options.configFile);

    result->hplmn.mcc = yaml::GetInt32(config, "mcc", 1, 999);
    yaml::GetString(config, "mcc", 3, 3);
    result->hplmn.mnc = yaml::GetInt32(config, "mnc", 0, 999);
    result->hplmn.isLongMnc = yaml::GetString(config, "mnc", 2, 3).size() == 3;
    if (yaml::HasField(config, "routingIndicator"))
        result->routingIndicator = yaml::GetString(config, "routingIndicator", 1, 4);

    for (auto &gnbSearchItem : yaml::GetSequence(config, "gnbSearchList"))
        result->gnbSearchList.push_back(gnbSearchItem.as<std::string>());
    if (result->gnbSearchList.empty())
        throw std::runtime_error("gnbSearchList must contain at least one gNB or simulated AP address");

    if (yaml::HasField(config, "default-nssai"))
    {
        for (auto &sNssai : yaml::GetSequence(config, "default-nssai"))
        {
            SingleSlice s{};
            s.sst = yaml::GetInt32(sNssai, "sst", 0, 0xFF);
            if (yaml::HasField(sNssai, "sd"))
                s.sd = octet3{yaml::GetInt32(sNssai, "sd", 0, 0xFFFFFF)};
            result->defaultConfiguredNssai.slices.push_back(s);
        }
    }

    if (yaml::HasField(config, "configured-nssai"))
    {
        for (auto &sNssai : yaml::GetSequence(config, "configured-nssai"))
        {
            SingleSlice s{};
            s.sst = yaml::GetInt32(sNssai, "sst", 0, 0xFF);
            if (yaml::HasField(sNssai, "sd"))
                s.sd = octet3{yaml::GetInt32(sNssai, "sd", 0, 0xFFFFFF)};
            result->configuredNssai.slices.push_back(s);
        }
    }

    result->key = OctetString::FromHex(yaml::GetString(config, "key", 32, 32));
    result->opC = OctetString::FromHex(yaml::GetString(config, "op", 32, 32));
    result->amf = OctetString::FromHex(yaml::GetString(config, "amf", 4, 4));

    result->configureRouting = !g_options.noRoutingConfigs;

    // If we have multiple UEs in the same process, then log names should be separated.
    result->prefixLogger = g_options.count > 1;

    if (yaml::HasField(config, "supi"))
        result->supi = Supi::Parse(yaml::GetString(config, "supi"));
    if (yaml::HasField(config, "protectionScheme"))
        result->protectionScheme = yaml::GetInt32(config, "protectionScheme", 0, 255);
    if (yaml::HasField(config, "homeNetworkPublicKeyId"))
        result->homeNetworkPublicKeyId = yaml::GetInt32(config, "homeNetworkPublicKeyId", 0, 255);
    if (yaml::HasField(config, "homeNetworkPublicKey"))        
        result->homeNetworkPublicKey = OctetString::FromHex(yaml::GetString(config, "homeNetworkPublicKey", 64, 64)); 
    if (yaml::HasField(config, "imei"))
        result->imei = yaml::GetString(config, "imei", 15, 15);
    if (yaml::HasField(config, "imeiSv"))
        result->imeiSv = yaml::GetString(config, "imeiSv", 16, 16);
    if (yaml::HasField(config, "tunName"))
        result->tunName = yaml::GetString(config, "tunName", 1, 12);

    yaml::AssertHasField(config, "integrity");
    yaml::AssertHasField(config, "ciphering");

    result->supportedAlgs.nia1 = yaml::GetBool(config["integrity"], "IA1");
    result->supportedAlgs.nia2 = yaml::GetBool(config["integrity"], "IA2");
    result->supportedAlgs.nia3 = yaml::GetBool(config["integrity"], "IA3");
    result->supportedAlgs.nea1 = yaml::GetBool(config["ciphering"], "EA1");
    result->supportedAlgs.nea2 = yaml::GetBool(config["ciphering"], "EA2");
    result->supportedAlgs.nea3 = yaml::GetBool(config["ciphering"], "EA3");

    std::string opType = yaml::GetString(config, "opType");
    if (opType == "OP")
        result->opType = nr::ue::OpType::OP;
    else if (opType == "OPC")
        result->opType = nr::ue::OpType::OPC;
    else
        throw std::runtime_error("Invalid OP type: " + opType);

    if (yaml::HasField(config, "sessions"))
    {
        for (auto &sess : yaml::GetSequence(config, "sessions"))
        {
            nr::ue::SessionConfig s{};

            if (yaml::HasField(sess, "apn"))
                s.apn = yaml::GetString(sess, "apn");
            if (yaml::HasField(sess, "slice"))
            {
                auto slice = sess["slice"];
                s.sNssai = SingleSlice{};
                s.sNssai->sst = yaml::GetInt32(slice, "sst", 0, 0xFF);
                if (yaml::HasField(slice, "sd"))
                    s.sNssai->sd = octet3{yaml::GetInt32(slice, "sd", 0, 0xFFFFFF)};
            }

            std::string type = yaml::GetString(sess, "type");
            if (type == "IPv4")
                s.type = nas::EPduSessionType::IPV4;
            else if (type == "IPv6")
                s.type = nas::EPduSessionType::IPV6;
            else if (type == "IPv4v6")
                s.type = nas::EPduSessionType::IPV4V6;
            else if (type == "Ethernet")
                s.type = nas::EPduSessionType::ETHERNET;
            else if (type == "Unstructured")
                s.type = nas::EPduSessionType::UNSTRUCTURED;
            else
                throw std::runtime_error("Invalid PDU session type: " + type);

            s.isEmergency = false;

            result->defaultSessions.push_back(s);
        }
    }

    yaml::AssertHasField(config, "integrityMaxRate");
    {
        auto uplink = yaml::GetString(config["integrityMaxRate"], "uplink");
        auto downlink = yaml::GetString(config["integrityMaxRate"], "downlink");
        if (uplink != "full" && uplink != "64kbps")
            throw std::runtime_error("Invalid integrity protection maximum uplink data rate: " + uplink);
        if (downlink != "full" && downlink != "64kbps")
            throw std::runtime_error("Invalid integrity protection maximum downlink data rate: " + downlink);
        result->integrityMaxRate.uplinkFull = uplink == "full";
        result->integrityMaxRate.downlinkFull = downlink == "full";
    }

    yaml::AssertHasField(config, "uacAic");
    {
        result->uacAic.mps = yaml::GetBool(config["uacAic"], "mps");
        result->uacAic.mcs = yaml::GetBool(config["uacAic"], "mcs");
    }

    yaml::AssertHasField(config, "uacAcc");
    {
        result->uacAcc.normalCls = yaml::GetInt32(config["uacAcc"], "normalClass", 0, 9);
        result->uacAcc.cls11 = yaml::GetBool(config["uacAcc"], "class11");
        result->uacAcc.cls12 = yaml::GetBool(config["uacAcc"], "class12");
        result->uacAcc.cls13 = yaml::GetBool(config["uacAcc"], "class13");
        result->uacAcc.cls14 = yaml::GetBool(config["uacAcc"], "class14");
        result->uacAcc.cls15 = yaml::GetBool(config["uacAcc"], "class15");
    }
    if (yaml::HasField(config, "phyLocation"))
    {
        const auto location = config["phyLocation"];
        const auto x = location["x"];
        const auto y = location["y"];
        const auto z = location["z"];
        if (!location.IsMap() || !x.IsSequence() || !y.IsSequence() || !z.IsSequence() || x.size() == 0 ||
            x.size() != y.size() || x.size() != z.size())
            throw std::runtime_error("phyLocation must contain nonempty x, y, z arrays of equal length");
        for (size_t i = 0; i < x.size(); ++i)
            result->phyLocations.emplace_back(x[i].as<int>(), y[i].as<int>(), z[i].as<int>());
    }
    else
        result->phyLocations.emplace_back(0, 0, 0);

    if (yaml::HasField(config, "pattern"))
        result->mobPattern = yaml::GetString(config, "pattern");
    if (result->mobPattern != "linear" && result->mobPattern != "loop")
        throw std::runtime_error("pattern must be 'linear' or 'loop'");

    // velocity is metres/second; mobility is the key used by the earlier UE fork.
    if (yaml::HasField(config, "velocity"))
        result->velocity = config["velocity"].as<float>();
    else if (yaml::HasField(config, "mobility"))
        result->velocity = config["mobility"].as<float>();
    if (!std::isfinite(result->velocity) || result->velocity < 0.0f)
        throw std::runtime_error("velocity (or legacy mobility) must be a finite nonnegative speed in metres/second");

    if (yaml::HasField(config, "trafficShapingInterface"))
        result->trafficShapingInterface = ReadInterface(config, "trafficShapingInterface");
    if (yaml::HasField(config, "wifi"))
        result->wifi = yaml::GetBool(config, "wifi");
    if (yaml::HasField(config, "wifiRoutingTable"))
        result->wifiRoutingTable = yaml::GetInt32(config, "wifiRoutingTable", 10000, 60000);
    if (result->wifi)
    {
        result->staticIP = yaml::GetString(config, "staticIP");
        in_addr address{};
        if (inet_pton(AF_INET, result->staticIP.c_str(), &address) != 1)
            throw std::runtime_error("staticIP must be an IPv4 address in Wi-Fi mode");
        result->interface = ReadInterface(config, "Interface");
    }
    return result;
}

static void ReadOptions(int argc, char **argv)
{
    opt::OptionsDescription desc{
        cons::Project, cons::Tag, "5G-SA UE implementation", cons::Owner, "nr-ue", {"-c <config-file> [option...]"}, {},
        true,          false};

    opt::OptionItem itemConfigFile = {'c', "config", "Use specified configuration file for UE", "config-file"};
    opt::OptionItem itemImsi = {'i', "imsi", "Use specified IMSI number instead of provided one", "imsi"};
    opt::OptionItem itemCount = {'n', "num-of-UE", "Generate specified number of UEs starting from the given IMSI",
                                 "num"};
    opt::OptionItem itemTempo = {'t', "tempo", "Starting delay in milliseconds for each of the UEs", "tempo"};
    opt::OptionItem itemDisableCmd = {'l', "disable-cmd", "Disable command line functionality for this instance",
                                      std::nullopt};
    opt::OptionItem itemDisableRouting = {'r', "no-routing-config",
                                          "Do not auto configure routing for UE TUN interface", std::nullopt};
    opt::OptionItem itemCheckConfig = {'C', "check-config", "Validate configuration without starting a node",
                                      std::nullopt};

    desc.items.push_back(itemConfigFile);
    desc.items.push_back(itemImsi);
    desc.items.push_back(itemCount);
    desc.items.push_back(itemTempo);
    desc.items.push_back(itemDisableCmd);
    desc.items.push_back(itemDisableRouting);
    desc.items.push_back(itemCheckConfig);

    opt::OptionsResult opt{argc, argv, desc, false, nullptr};

    g_options.configFile = opt.getOption(itemConfigFile);
    g_options.noRoutingConfigs = opt.hasFlag(itemDisableRouting);
    g_options.checkConfig = opt.hasFlag(itemCheckConfig);
    if (opt.hasFlag(itemCount))
    {
        g_options.count = utils::ParseInt(opt.getOption(itemCount));
        if (g_options.count <= 0)
            throw std::runtime_error("Invalid number of UEs");
        if (g_options.count > 512)
            throw std::runtime_error("Number of UEs is too big");
    }
    else
    {
        g_options.count = 1;
    }

    if (opt.hasFlag(itemTempo))
        g_options.tempo = utils::ParseInt(opt.getOption(itemTempo));
    else
        g_options.tempo = 0;

    g_options.imsi = {};
    if (opt.hasFlag(itemImsi))
    {
        g_options.imsi = opt.getOption(itemImsi);
        if (g_options.imsi.length() > 5 && g_options.imsi[0] == 'i' && g_options.imsi[1] == 'm' &&
            g_options.imsi[2] == 's' && g_options.imsi[3] == 'i' && g_options.imsi[4] == '-')
        {
            g_options.imsi = g_options.imsi.substr(5);
        }

        Supi::Parse("imsi-" + g_options.imsi); // validate the string by parsing
    }

    g_options.disableCmd = opt.hasFlag(itemDisableCmd);
}

static std::string LargeSum(std::string a, std::string b)
{
    if (a.length() > b.length())
        std::swap(a, b);

    std::string str;
    size_t n1 = a.length(), n2 = b.length();

    reverse(a.begin(), a.end());
    reverse(b.begin(), b.end());

    int carry = 0;
    for (size_t i = 0; i < n1; i++)
    {
        int sum = ((a[i] - '0') + (b[i] - '0') + carry);
        str.push_back(static_cast<char>((sum % 10) + '0'));
        carry = sum / 10;
    }
    for (size_t i = n1; i < n2; i++)
    {
        int sum = ((b[i] - '0') + carry);
        str.push_back(static_cast<char>((sum % 10) + '0'));
        carry = sum / 10;
    }
    if (carry)
        throw std::runtime_error("UE serial number overflow");
    reverse(str.begin(), str.end());
    return str;
}

static void IncrementNumber(std::string &s, int delta)
{
    s = LargeSum(s, std::to_string(delta));
}

static nr::ue::UeConfig *GetConfigByUe(int ueIndex)
{
    auto *c = new nr::ue::UeConfig();
    c->key = g_refConfig->key.copy();
    c->opC = g_refConfig->opC.copy();
    c->opType = g_refConfig->opType;
    c->amf = g_refConfig->amf.copy();
    c->imei = g_refConfig->imei;
    c->imeiSv = g_refConfig->imeiSv;
    c->supi = g_refConfig->supi;
    c->protectionScheme = g_refConfig->protectionScheme;
    c->homeNetworkPublicKey = g_refConfig->homeNetworkPublicKey.copy();
    c->homeNetworkPublicKeyId = g_refConfig->homeNetworkPublicKeyId;
    c->routingIndicator = g_refConfig->routingIndicator;
    c->tunName = g_refConfig->tunName;
    c->hplmn = g_refConfig->hplmn;
    c->configuredNssai = g_refConfig->configuredNssai;
    c->defaultConfiguredNssai = g_refConfig->defaultConfiguredNssai;
    c->supportedAlgs = g_refConfig->supportedAlgs;
    c->gnbSearchList = g_refConfig->gnbSearchList;
    c->defaultSessions = g_refConfig->defaultSessions;
    c->configureRouting = g_refConfig->configureRouting;
    c->prefixLogger = g_refConfig->prefixLogger;
    c->integrityMaxRate = g_refConfig->integrityMaxRate;
    c->uacAic = g_refConfig->uacAic;
    c->uacAcc = g_refConfig->uacAcc;
    c->phyLocations = g_refConfig->phyLocations;
    c->mobPattern = g_refConfig->mobPattern;
    c->velocity = g_refConfig->velocity;
    c->wifi = g_refConfig->wifi;
    c->staticIP = g_refConfig->staticIP;
    c->interface = g_refConfig->interface;
    c->trafficShapingInterface = g_refConfig->trafficShapingInterface;
    c->wifiRoutingTable = g_refConfig->wifiRoutingTable;
    if (c->supi.has_value())
        IncrementNumber(c->supi->value, ueIndex);
    if (c->imei.has_value())
        IncrementNumber(*c->imei, ueIndex);
    if (c->imeiSv.has_value())
        IncrementNumber(*c->imeiSv, ueIndex);

    return c;
}

static void ReceiveCommand(app::CliMessage &msg)
{
    if (msg.value.empty())
    {
        g_cliServer->sendMessage(app::CliMessage::Result(msg.clientAddr, ""));
        return;
    }

    std::vector<std::string> tokens{};

    auto exp = opt::PerformExpansion(msg.value, tokens);
    if (exp != opt::ExpansionResult::SUCCESS)
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, "Invalid command: " + msg.value));
        return;
    }

    if (tokens.empty())
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, "Empty command"));
        return;
    }

    std::string error{}, output{};
    auto cmd = app::ParseUeCliCommand(std::move(tokens), error, output);
    if (!error.empty())
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, error));
        return;
    }
    if (!output.empty())
    {
        g_cliServer->sendMessage(app::CliMessage::Result(msg.clientAddr, output));
        return;
    }
    if (cmd == nullptr)
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, ""));
        return;
    }

    auto *ue = g_ueMap.getOrDefault(msg.nodeName);
    if (ue == nullptr)
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, "Node not found: " + msg.nodeName));
        return;
    }

    ue->pushCommand(std::move(cmd), msg.clientAddr);
}

static void Loop()
{
    if (!g_cliServer)
    {
        utils::Sleep(100);
        return;
    }

    auto msg = g_cliServer->receiveMessage();
    if (msg.type == app::CliMessage::Type::ECHO)
    {
        g_cliServer->sendMessage(msg);
        return;
    }

    if (msg.type != app::CliMessage::Type::COMMAND)
        return;

    if (msg.value.size() > 0xFFFF)
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, "Command is too large"));
        return;
    }

    if (msg.nodeName.size() > 0xFFFF)
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, "Node name is too large"));
        return;
    }

    ReceiveCommand(msg);
}

static class UeController : public app::IUeController
{
  public:
    void performSwitchOff(nr::ue::UserEquipment *ue) override
    {
        auto w = std::make_unique<NwUeControllerCmd>(NwUeControllerCmd::PERFORM_SWITCH_OFF);
        w->ue = ue;
        g_controllerTask->push(std::move(w));
    }
} g_ueController;

int main(int argc, char **argv)
{
    try
    {
        ReadOptions(argc, argv);
        g_refConfig = ReadConfigYaml();
        if (g_refConfig->wifi && g_options.count > 1)
            throw std::runtime_error("Wi-Fi mode requires one UE per process with its own staticIP and wifiRoutingTable");
        if (g_options.imsi.length() > 0)
            g_refConfig->supi = Supi::Parse("imsi-" + g_options.imsi);
        if (g_options.checkConfig)
        {
            std::cout << "Configuration valid" << std::endl;
            delete g_refConfig;
            return 0;
        }
        CheckWifiNetwork(*g_refConfig);
    }
    catch (const std::runtime_error &e)
    {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }

    app::Initialize();
    std::cout << cons::Name << std::endl;

    // Build all nodes and their sockets before starting threads, so startup failures exit cleanly.
    try
    {
        g_controllerTask = new UeControllerTask();
        if (!g_options.disableCmd)
        {
            g_cliServer = new app::CliServer{};
            g_cliRespTask = new app::CliResponseTask(g_cliServer);
        }

        for (int i = 0; i < g_options.count; i++)
        {
            auto *config = GetConfigByUe(i);
            auto *ue = new nr::ue::UserEquipment(config, &g_ueController, nullptr, g_cliRespTask);
            g_ueMap.put(config->getNodeName(), ue);
        }
    }
    catch (const std::exception &error)
    {
        std::cerr << "ERROR: UE startup failed: " << error.what() << std::endl;
        g_ueMap.invokeForeach([](const auto &entry) { delete entry.second; });
        delete g_cliRespTask;
        delete g_cliServer;
        delete g_controllerTask;
        delete g_refConfig;
        app::Finalize();
        return 1;
    }
    g_controllerTask->start();

    if (!g_options.disableCmd)
    {
        app::CreateProcTable(g_ueMap, g_cliServer->assignedAddress().getPort());
        g_cliRespTask->start();
    }

    if (g_options.tempo != 0)
    {
        g_ueMap.invokeForeach([](const auto &ue) {
            ue.second->start();
            std::this_thread::sleep_for(std::chrono::milliseconds(g_options.tempo));
        });
    }
    else
    {
        g_ueMap.invokeForeach([](const auto &ue) { ue.second->start(); });
    }

    while (!app::ShutdownRequested())
        Loop();
    g_controllerTask->quit();
    delete g_controllerTask;
    g_ueMap.invokeForeach([](const auto &entry) { delete entry.second; });
    if (g_cliRespTask)
    {
        g_cliRespTask->quit();
        delete g_cliRespTask;
    }
    delete g_cliServer;
    delete g_refConfig;
    app::Finalize();
    return app::ExitCode();
}
