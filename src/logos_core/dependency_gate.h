#ifndef LOGOS_DEPENDENCY_GATE_H
#define LOGOS_DEPENDENCY_GATE_H

#include <functional>
#include <string>
#include <vector>

// Dependency version-range load gate: the decision only. Its inputs are
// decoded by logos-module (ModuleLib::ModuleMetadata) and mapped onto the
// types below in module_registry.cpp, the Qt-aware TU -- this header stays
// std-only.
//
// Like the protocol gate this is a COMPATIBILITY control, not a security one:
// range and installed version are both self-asserted plugin metadata. The
// signer pin is carried but NOT enforced — see module_manager.cpp.
//
// It fails CLOSED: a range liblgx cannot parse, or one it cannot evaluate for
// want of a readable installed version, refuses the load. An unenforceable
// constraint is the exact fail-open this gate removes. An entry declaring no
// range is not gated at all, which is every module in the fleet today.
//
// liblgx accepts ^ ~ x-wildcard * and comparator sets, but NOT npm hyphen
// ranges ("1.2.3 - 2.0.0"), and a prerelease ("1.0.0-dev") satisfies no caret
// range — both therefore refuse rather than pass.

namespace LogosCore {

struct ModuleDependency {
    std::string name;
    std::string versionRange;  // empty when the entry declares no range
    std::string signer;        // empty when the entry pins no signer
    // A `version`/`signer` that is present but not a string. Declaring a
    // constraint we cannot read is not the same as declaring none, so it
    // refuses rather than falling through to the unconstrained arm.
    bool malformedConstraint = false;
};

enum class DependencyGateDecision {
    Allow,          // every declared range is satisfied
    Unconstrained,  // no entry declared a range — pre-constraint module
    Refuse,
};

struct DependencyGateResult {
    DependencyGateDecision decision = DependencyGateDecision::Unconstrained;
    // Populated on Refuse only; installedVersion is empty when unreadable.
    std::string dependency;
    std::string range;
    std::string installedVersion;
    std::string reason;
};

// Evaluates the direct edges of one module. Transitive coverage falls out of
// gating every module as it loads. `installedVersionOf` returns "" for a
// dependency that is unknown or carries no version.
DependencyGateResult evaluateDependencyGate(
    const std::vector<ModuleDependency>& dependencies,
    const std::function<std::string(const std::string&)>& installedVersionOf);

} // namespace LogosCore

#endif // LOGOS_DEPENDENCY_GATE_H
