#ifndef MODULE_STATE_OBSERVER_H
#define MODULE_STATE_OBSERVER_H

// The module-lifecycle observer.
//
// The one place in liblogos that turns "a module changed state" into a
// structured, sequenced fact. Today load/unload/crash are spdlog::info lines
// and registry membership changes are silent, so every consumer polls.
//
// It does not talk to any module and does not know `modules_state` exists — it
// hands transitions to a SINK. With no sink installed this is nearly free:
// record() early-outs before it allocates or takes a lock, which matters
// because a host with no consumer is the normal case.
//
// ── RULE 1: NEVER DISPATCH UNDER loadMutex() ─────────────────────────────────
//
// The seam points run with ModuleManager's loadMutex() held. A sink that
// performs an RPC — which is exactly what the real one does — would then make a
// synchronous outbound call from inside the load path while holding the lock
// every other load and unload needs. That is the shape of two failures already
// paid for here: the ui-host startup token deadlock, and a ~417 s Basecamp
// stall from a synchronous call to a module that was not there.
//
// So: record() under the caller's lock buffers; flush() after releasing it
// dispatches. See ScopedModuleStateFlush.
//
// ── RULE 2: ONE SEQ COUNTER, FOR DELTAS AND SNAPSHOTS ────────────────────────
//
// modules_state applies a transition iff its seq beats what it stored for that
// module, and keeps a seq TOMBSTONE for one that left the listing. That puts a
// requirement on this side which is invisible from over there: snapshot record
// seqs must come from the SAME counter as transition seqs. Anything else makes
// the tombstone unreachably high (a real later delta dropped forever) or
// trivially low (a stale delta resurrecting a pruned module). nextSeq() is that
// counter and there is no other.
//
// THREAD SAFETY: load/unload run on whichever thread called the C API; a module
// DYING is detected on the container's background asio thread, not under
// loadMutex() at all. Every method here is safe from any thread.

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace logos {

// Mirrors modules_state. Free constants rather than an enum: the wire type is
// `tstr`, and the contract requires an unrecognised state to be tolerated.
// kAbsent is EVENT-ONLY — a transition endpoint, never a resting state.
namespace module_state {
constexpr const char* kAbsent   = "absent";
constexpr const char* kUnloaded = "unloaded";
constexpr const char* kLoading  = "loading";
constexpr const char* kLoaded   = "loaded";
// kLoaded means the host owns the process. kReady means the module's object is
// acquirable -- observed asynchronously, since publish happens in the child.
constexpr const char* kReady    = "ready";
constexpr const char* kStopping = "stopping";
constexpr const char* kError    = "error";
}  // namespace module_state

// One lifecycle transition, already sequenced. instance is the host's
// persistence identity and is STABLE across load/unload cycles, so only a
// changed pid tells a consumer a module died and came back.
struct ModuleTransition {
    std::string module;
    std::optional<std::string> instance;
    std::optional<int64_t> pid;
    std::string oldState;
    std::string newState;
    std::optional<std::string> reason;
    uint64_t seq = 0;
};

class ModuleStateObserver {
public:
    // Called with a batch, always OUTSIDE loadMutex(). Must not block: it runs
    // on the thread that performed the load, unload or termination.
    using Sink = std::function<void(const std::vector<ModuleTransition>&)>;

    static ModuleStateObserver& instance();

    // Install (or clear, with a default-constructed Sink) the consumer.
    void setSink(Sink sink);

    // True when a sink is installed. Callers may use this to skip building
    // arguments they would otherwise throw away.
    bool hasSink() const;

    // The single monotonic counter — see rule 2.
    uint64_t nextSeq();

    // Buffer one transition. Allocation-free with no sink installed, safe with
    // any caller lock held, and does NOT dispatch. A no-op (old == new) is
    // dropped here so old_state is never equal to new_state at the sink.
    void record(const std::string& module,
                const std::string& oldState,
                const std::string& newState,
                std::optional<std::string> instance = std::nullopt,
                std::optional<int64_t> pid = std::nullopt,
                std::optional<std::string> reason = std::nullopt);

    // Dispatch everything buffered so far. MUST be called with no caller lock
    // held. Safe to call when nothing is buffered.
    void flush();

    // Drop anything buffered without dispatching. For tests.
    void clearPending();

private:
    ModuleStateObserver() = default;

    mutable std::mutex m_mutex;
    std::vector<ModuleTransition> m_pending;
    Sink m_sink;
    uint64_t m_seq = 0;
};

// Flushes on scope exit. Declare it BEFORE the lock guard so it is destroyed
// AFTER it; the other order dispatches under the lock, which rule 1 forbids.
struct ScopedModuleStateFlush {
    ~ScopedModuleStateFlush() { ModuleStateObserver::instance().flush(); }
};

}  // namespace logos

#endif  // MODULE_STATE_OBSERVER_H
