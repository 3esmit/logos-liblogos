#include "dependency_gate.h"

#include <lgx.h>

namespace LogosCore {

DependencyGateResult evaluateDependencyGate(
    const std::vector<ModuleDependency>& dependencies,
    const std::function<std::string(const std::string&)>& installedVersionOf)
{
    DependencyGateResult out;

    for (const ModuleDependency& dep : dependencies) {
        if (dep.malformedConstraint) {
            out.decision = DependencyGateDecision::Refuse;
            out.dependency = dep.name;
            out.reason = "dependency " + dep.name +
                         " declares a constraint that is not a string";
            return out;
        }
        if (dep.versionRange.empty())
            continue;

        const std::string installed =
            installedVersionOf ? installedVersionOf(dep.name) : std::string();

        const char* problem = nullptr;
        if (!lgx_semver_valid_range(dep.versionRange.c_str()))
            problem = "declares an unparseable version range";
        else if (installed.empty())
            problem = "has no readable installed version";
        else if (!lgx_semver_satisfies(installed.c_str(), dep.versionRange.c_str()))
            problem = "is out of range";

        if (problem) {
            out.decision = DependencyGateDecision::Refuse;
            out.dependency = dep.name;
            out.range = dep.versionRange;
            out.installedVersion = installed;
            out.reason = "dependency " + dep.name + " " + problem + ": requires " +
                         dep.versionRange + ", installed " +
                         (installed.empty() ? std::string("(unknown)") : installed);
            return out;
        }
        out.decision = DependencyGateDecision::Allow;
    }

    return out;
}

} // namespace LogosCore
