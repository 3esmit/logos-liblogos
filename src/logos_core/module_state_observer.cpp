#include "module_state_observer.h"

#include <utility>

namespace logos {

ModuleStateObserver& ModuleStateObserver::instance()
{
    // One observer per liblogos IMAGE. On PE that means one per image — the
    // trap that gave Basecamp three TokenManagers — and it is acceptable here
    // only because this holds no cross-image contract: a second observer with
    // no sink does nothing, whereas a second TokenManager loses tokens.
    static ModuleStateObserver s_instance;
    return s_instance;
}

void ModuleStateObserver::setSink(Sink sink)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sink = std::move(sink);
}

bool ModuleStateObserver::hasSink() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<bool>(m_sink);
}

uint64_t ModuleStateObserver::nextSeq()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ++m_seq;
}

void ModuleStateObserver::record(const std::string& module,
                                 const std::string& oldState,
                                 const std::string& newState,
                                 std::optional<std::string> instance,
                                 std::optional<int64_t> pid,
                                 std::optional<std::string> reason)
{
    // A no-op is not a transition. Dropped here so the guarantee holds for
    // every consumer rather than being re-enforced at each one.
    if (module.empty() || oldState.empty() || newState.empty())
        return;
    if (oldState == newState)
        return;

    ModuleTransition t;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // No sink means nobody is listening; buffering would be an unbounded
        // leak in the normal case. Checked under the lock so it cannot race
        // setSink().
        if (!m_sink)
            return;

        t.seq = ++m_seq;
        t.module = module;
        t.oldState = oldState;
        t.newState = newState;
        t.instance = std::move(instance);
        t.pid = std::move(pid);
        t.reason = std::move(reason);
        m_pending.push_back(std::move(t));
    }
}

void ModuleStateObserver::flush()
{
    // Compute under the lock, dispatch after releasing it — rule 1. The sink is
    // copied out too: calling it under m_mutex would deadlock the moment a sink
    // re-entered record(), which is an ordinary thing to write.
    std::vector<ModuleTransition> batch;
    Sink sink;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending.empty() || !m_sink)
            return;
        batch.swap(m_pending);
        sink = m_sink;
    }
    sink(batch);
}

void ModuleStateObserver::clearPending()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.clear();
}

}  // namespace logos
