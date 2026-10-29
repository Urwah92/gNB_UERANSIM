#include <ue/rls/ctl_task.hpp>
#include <ue/rrc/task.hpp>
#include <utils/common.hpp>

#include <iostream>
#include <stdexcept>

namespace nr::ue
{
struct UeHandoverTest
{
    static void require(bool condition, const char *message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    static void run()
    {
        UeConfig config{};
        LogBase log("ue-handover-test.log");
        TaskBase base{};
        base.config = &config;
        base.logBase = &log;
        RlsSharedContext shared{};
        RlsControlTask control(&base, &shared);
        const auto now = utils::CurrentTimeMillis();
        control.m_pendingHandovers[1] = {10, 100, 20, now};
        control.m_pendingHandovers[2] = {11, 101, 21, now};

        // These paths must not release the source or emit a cell-switch event.
        // Leave UDP/main tasks absent so any accidental completion fails the test.
        rls::RlsXnSessionTransmission completion(1);
        completion.payload = 1;
        control.handleRlsMessage(21, 0, completion);
        require(control.m_pendingHandovers.size() == 2, "wrong target must not consume pending handover");
        completion.payload = 3;
        control.handleRlsMessage(20, 0, completion);
        require(control.m_pendingHandovers.size() == 2, "unknown PSI must not consume pending handover");

        rls::RlsSessionTransmission overlapping(1);
        overlapping.pduId = 999;
        overlapping.m_pduSession = std::make_unique<nr::gnb::PduSessionResource>(999, 1);
        control.handleRlsMessage(30, 40, overlapping);
        require(control.m_pendingHandovers.at(1).sourceUeId == 100 &&
                    control.m_pendingHandovers.at(1).targetCell == 20,
                "overlapping transfer must preserve original source/target correlation");

        control.m_pendingHandovers.at(1).sentAt = now - 10001;
        completion.payload = 1;
        control.handleRlsMessage(20, 0, completion);
        require(control.m_pendingHandovers.size() == 2, "late completion must not release source");
        control.onAckControlTimerExpired();
        require(control.m_pendingHandovers.size() == 1 && control.m_pendingHandovers.count(2),
                "expiry must leave other PDU sessions pending");

        // Each PDU session may acknowledge the same target separately. A second switch must
        // retain the target descriptor and avoid sending another NAS/RLS transition.
        UeRrcTask rrc(&base);
        rrc.m_cellDesc[20] = {};
        base.shCtx.currentCell.mutate([](auto &cell) { cell.cellId = 20; });
        rrc.handoverToTargetCellPreservingContext(20);
        rrc.handoverToTargetCellPreservingContext(20);
        require(rrc.m_cellDesc.count(20) && base.shCtx.currentCell.get().cellId == 20,
                "repeated target completion must preserve active target cell");
    }
};
} // namespace nr::ue

int main()
{
    try
    {
        nr::ue::UeHandoverTest::run();
        std::cout << "UE handover tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
