#include <gtest/gtest.h>

#include "logos_core/module_state_observer.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

// Each case guards one of the two rules in module_state_observer.h, or a
// guarantee modules_state relies on at the far end of the wire.

using logos::ModuleStateObserver;
using logos::ModuleTransition;
namespace ms = logos::module_state;

namespace {

// The observer is a process-wide singleton, so each case installs and removes
// its own sink rather than leaking one into the next.
class ObserverFixture : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto& o = ModuleStateObserver::instance();
        o.setSink({});
        o.clearPending();
        batches = 0;
        seen.clear();
        o.setSink([this](const std::vector<ModuleTransition>& batch) {
            ++batches;
            for (const auto& t : batch)
                seen.push_back(t);
        });
    }

    void TearDown() override
    {
        auto& o = ModuleStateObserver::instance();
        o.setSink({});
        o.clearPending();
    }

    std::vector<ModuleTransition> seen;
    int batches = 0;
};

}  // namespace

// record() BUFFERS. It must not call the sink, because at the seam points it is
// invoked with loadMutex() held — the whole reason flush() is separate.
TEST_F(ObserverFixture, RecordDoesNotDispatch)
{
    auto& o = ModuleStateObserver::instance();
    o.record("chat_module", ms::kUnloaded, ms::kLoading);
    o.record("chat_module", ms::kLoading, ms::kLoaded);

    EXPECT_TRUE(seen.empty());
    EXPECT_EQ(batches, 0);

    o.flush();

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(batches, 1);
    EXPECT_EQ(seen[0].newState, ms::kLoading);
    EXPECT_EQ(seen[1].newState, ms::kLoaded);
}

// Strictly increasing and shared across every module: modules_state tombstones
// a departed record at a seq, so a per-module or restarting counter would make
// that tombstone unreachably high or trivially low.
TEST_F(ObserverFixture, SeqIsGloballyMonotonic)
{
    auto& o = ModuleStateObserver::instance();
    o.record("a", ms::kUnloaded, ms::kLoading);
    o.record("b", ms::kUnloaded, ms::kLoading);
    o.record("a", ms::kLoading, ms::kLoaded);
    o.flush();

    ASSERT_EQ(seen.size(), 3u);
    EXPECT_LT(seen[0].seq, seen[1].seq);
    EXPECT_LT(seen[1].seq, seen[2].seq);

    // nextSeq() draws from the SAME counter — this is what makes a snapshot's
    // record seqs comparable with delta seqs on the modules_state side.
    const uint64_t afterBatch = seen[2].seq;
    EXPECT_GT(o.nextSeq(), afterBatch);
}

// old == new is not a transition. modules_state refuses them anyway, so
// emitting them would be defined-to-be-discarded traffic.
TEST_F(ObserverFixture, NoOpTransitionsAreDropped)
{
    auto& o = ModuleStateObserver::instance();
    o.record("chat_module", ms::kLoaded, ms::kLoaded);
    o.record("chat_module", "", ms::kLoaded);
    o.record("chat_module", ms::kLoaded, "");
    o.record("", ms::kUnloaded, ms::kLoaded);
    o.flush();

    EXPECT_TRUE(seen.empty());
}

// With no sink, record() must not accumulate — a host with no consumer is the
// NORMAL case, so buffering there is an unbounded leak.
TEST_F(ObserverFixture, NothingBuffersWithoutASink)
{
    auto& o = ModuleStateObserver::instance();
    o.setSink({});

    for (int i = 0; i < 10000; ++i)
        o.record("chat_module", ms::kUnloaded, ms::kLoading);

    // Re-install and flush: if the calls above had been buffered, they would
    // arrive now.
    o.setSink([this](const std::vector<ModuleTransition>& batch) {
        for (const auto& t : batch) seen.push_back(t);
    });
    o.flush();

    EXPECT_TRUE(seen.empty());
}

// The membership edges. `absent` is event-only and is the only way to name
// "the host discovered this" and "the host pruned this".
TEST_F(ObserverFixture, AbsentRidesAsATransitionTarget)
{
    auto& o = ModuleStateObserver::instance();
    o.record("waku_module", ms::kAbsent, ms::kUnloaded);
    o.record("waku_module", ms::kUnloaded, ms::kAbsent, std::nullopt, std::nullopt,
             "module files are no longer on disk");
    o.flush();

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].oldState, ms::kAbsent);
    EXPECT_EQ(seen[1].newState, ms::kAbsent);
    ASSERT_TRUE(seen[1].reason.has_value());
    EXPECT_NE(seen[1].reason->find("no longer on disk"), std::string::npos);
}

// instance and pid answer different questions and both must survive the trip:
// instance is stable across load/unload cycles, so only a changed pid can tell
// a consumer that a module died and came back.
TEST_F(ObserverFixture, InstanceAndPidAreCarried)
{
    auto& o = ModuleStateObserver::instance();
    o.record("chat_module", ms::kLoading, ms::kLoaded,
             std::string("inst-7"), int64_t{4242});
    o.flush();

    ASSERT_EQ(seen.size(), 1u);
    ASSERT_TRUE(seen[0].instance.has_value());
    EXPECT_EQ(*seen[0].instance, "inst-7");
    ASSERT_TRUE(seen[0].pid.has_value());
    EXPECT_EQ(*seen[0].pid, 4242);
}

// A sink that re-enters must not deadlock: flush() copies the batch and sink
// out, then releases the lock before calling.
TEST_F(ObserverFixture, SinkMayReenterTheObserver)
{
    auto& o = ModuleStateObserver::instance();
    o.setSink([&o](const std::vector<ModuleTransition>&) {
        // Re-entrant calls: these would self-deadlock if flush() dispatched
        // while still holding the observer's mutex.
        (void)o.hasSink();
        (void)o.nextSeq();
        o.record("reentrant_module", ms::kUnloaded, ms::kLoading);
    });

    o.record("chat_module", ms::kUnloaded, ms::kLoading);
    o.flush();   // must return; a hang here IS the failure

    SUCCEED();
}

// onTerminated fires on the container's background asio thread while loads run
// on the caller's thread, so record() is genuinely concurrent.
TEST_F(ObserverFixture, ConcurrentRecordersProduceUniqueSeqs)
{
    auto& o = ModuleStateObserver::instance();
    constexpr int kThreads = 8;
    constexpr int kPerThread = 250;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&o, t]() {
            for (int i = 0; i < kPerThread; ++i)
                o.record("m" + std::to_string(t), ms::kUnloaded, ms::kLoading);
        });
    }
    for (auto& th : threads)
        th.join();

    o.flush();

    ASSERT_EQ(seen.size(), static_cast<size_t>(kThreads * kPerThread));

    // Every seq distinct: a duplicate would make modules_state silently drop a
    // real transition, since it applies only strictly-greater seqs.
    std::vector<uint64_t> seqs;
    seqs.reserve(seen.size());
    for (const auto& t : seen)
        seqs.push_back(t.seq);
    std::sort(seqs.begin(), seqs.end());
    EXPECT_EQ(std::adjacent_find(seqs.begin(), seqs.end()), seqs.end());
}

// flush() with nothing buffered is a no-op, not an empty batch: the seam points
// flush unconditionally, and a sink filtering empty batches is one more thing
// to get wrong.
TEST_F(ObserverFixture, FlushWithNothingPendingDoesNotCallTheSink)
{
    ModuleStateObserver::instance().flush();
    EXPECT_EQ(batches, 0);
}
