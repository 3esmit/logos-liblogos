#include <gtest/gtest.h>

#include "logos_core/dependency_gate.h"

#include <map>
#include <string>

// The gate's decision table. Ranges are evaluated by liblgx's semver, so these
// also pin the npm dialect the manifests already use.

using LogosCore::DependencyGateDecision;
using LogosCore::evaluateDependencyGate;
using LogosCore::ModuleDependency;

namespace {

// Installed versions keyed by module name; a name that is absent stands for a
// dependency with no readable version.
auto installed(std::map<std::string, std::string> versions)
{
    return [versions = std::move(versions)](const std::string& name) {
        auto it = versions.find(name);
        return it != versions.end() ? it->second : std::string{};
    };
}

}  // namespace

// The string form of a dependency entry, and the object form that names no
// version, both declare an edge and constrain nothing.
TEST(DependencyGate, NoRangeIsUnconstrained)
{
    const std::vector<ModuleDependency> deps = {
        {"chat_module", "", ""},
        {"waku_module", "", "did:jwk:abc"},
    };
    const auto r = evaluateDependencyGate(deps, installed({{"chat_module", "1.0.0"}}));
    EXPECT_EQ(r.decision, DependencyGateDecision::Unconstrained);
    EXPECT_TRUE(r.reason.empty());
}

TEST(DependencyGate, SatisfiedRangeAllows)
{
    const std::vector<ModuleDependency> deps = {{"chat_module", "^2.0.0", ""}};
    const auto r = evaluateDependencyGate(deps, installed({{"chat_module", "2.1.0"}}));
    EXPECT_EQ(r.decision, DependencyGateDecision::Allow);
    EXPECT_TRUE(r.reason.empty());
}

TEST(DependencyGate, UnsatisfiedRangeRefuses)
{
    const std::vector<ModuleDependency> deps = {{"chat_module", "^2.0.0", ""}};
    const auto r = evaluateDependencyGate(deps, installed({{"chat_module", "1.0.0"}}));
    ASSERT_EQ(r.decision, DependencyGateDecision::Refuse);
    EXPECT_EQ(r.dependency, "chat_module");
    EXPECT_EQ(r.range, "^2.0.0");
    EXPECT_EQ(r.installedVersion, "1.0.0");
    EXPECT_NE(r.reason.find("^2.0.0"), std::string::npos);
    EXPECT_NE(r.reason.find("1.0.0"), std::string::npos);
}

// Fail closed: a constraint that cannot be parsed is the exact fail-open the
// gate exists to remove, so it refuses rather than being ignored.
TEST(DependencyGate, MalformedRangeRefuses)
{
    for (const char* bad : {"not a range", "^^1.0.0", "garbage!!"}) {
        const std::vector<ModuleDependency> deps = {{"chat_module", bad, ""}};
        const auto r = evaluateDependencyGate(deps, installed({{"chat_module", "1.0.0"}}));
        EXPECT_EQ(r.decision, DependencyGateDecision::Refuse) << bad;
        EXPECT_NE(r.reason.find("unparseable"), std::string::npos) << bad;
    }
}

// The dialect liblgx actually implements, pinned because the refusals surprise:
// npm hyphen ranges are not supported, and a prerelease satisfies no caret
// range. Both fail closed, so a module author sees a refusal, not a silent pass.
TEST(DependencyGate, RangeDialectLiblgxImplements)
{
    for (const char* ok : {"^2.0.0", "~2.1.0", "2.x", "*", "2.1.0", ">=2.0.0 <3.0.0"}) {
        const std::vector<ModuleDependency> deps = {{"chat_module", ok, ""}};
        EXPECT_EQ(evaluateDependencyGate(deps, installed({{"chat_module", "2.1.0"}})).decision,
                  DependencyGateDecision::Allow) << ok;
    }
    const std::vector<ModuleDependency> hyphen = {{"chat_module", "1.2.3 - 2.0.0", ""}};
    EXPECT_EQ(evaluateDependencyGate(hyphen, installed({{"chat_module", "1.5.0"}})).decision,
              DependencyGateDecision::Refuse);

    const std::vector<ModuleDependency> caret = {{"chat_module", "^1.0.0", ""}};
    EXPECT_EQ(evaluateDependencyGate(caret, installed({{"chat_module", "1.0.0-dev"}})).decision,
              DependencyGateDecision::Refuse);
}

// Same reason: a range declared against a dependency whose version cannot be
// read is unevaluatable, not satisfied.
TEST(DependencyGate, UnreadableInstalledVersionRefuses)
{
    const std::vector<ModuleDependency> deps = {{"chat_module", "^2.0.0", ""}};
    const auto r = evaluateDependencyGate(deps, installed({}));
    ASSERT_EQ(r.decision, DependencyGateDecision::Refuse);
    EXPECT_TRUE(r.installedVersion.empty());
    EXPECT_NE(r.reason.find("(unknown)"), std::string::npos);
}

TEST(DependencyGate, UnparseableInstalledVersionRefuses)
{
    const std::vector<ModuleDependency> deps = {{"chat_module", "^2.0.0", ""}};
    const auto r = evaluateDependencyGate(deps, installed({{"chat_module", "banana"}}));
    EXPECT_EQ(r.decision, DependencyGateDecision::Refuse);
}

// A mixed list refuses on the offending edge, and names that edge rather than
// the first one it walked.
TEST(DependencyGate, ReportsTheOffendingEdge)
{
    const std::vector<ModuleDependency> deps = {
        {"waku_module", "", ""},
        {"chat_module", ">=1.0.0", ""},
        {"storage_module", "~3.2.0", ""},
    };
    const auto r = evaluateDependencyGate(
        deps, installed({{"chat_module", "1.4.0"}, {"storage_module", "3.5.0"}}));
    ASSERT_EQ(r.decision, DependencyGateDecision::Refuse);
    EXPECT_EQ(r.dependency, "storage_module");
    EXPECT_EQ(r.installedVersion, "3.5.0");
}

// A signer pin alone is carried, never enforced — enforcement needs evidence
// lgpm does not persist. See the note at the enforcement point.
TEST(DependencyGate, SignerPinAloneIsNotEnforced)
{
    const std::vector<ModuleDependency> deps = {{"chat_module", "", "did:jwk:whoever"}};
    EXPECT_EQ(evaluateDependencyGate(deps, installed({{"chat_module", "1.0.0"}})).decision,
              DependencyGateDecision::Unconstrained);
}

// A constraint we cannot read is not a constraint we can ignore: the flag
// alone refuses, before any range is even looked at.
TEST(DependencyGate, MalformedConstraintRefuses)
{
    const std::vector<ModuleDependency> deps = {{"chat_module", "", "", true}};
    const auto r = evaluateDependencyGate(deps, installed({{"chat_module", "1.0.0"}}));
    EXPECT_EQ(r.decision, DependencyGateDecision::Refuse);
    EXPECT_EQ(r.dependency, "chat_module");
    EXPECT_NE(r.reason.find("not a string"), std::string::npos) << r.reason;
}

TEST(DependencyGate, NoDependenciesIsUnconstrained)
{
    EXPECT_EQ(evaluateDependencyGate({}, installed({})).decision,
              DependencyGateDecision::Unconstrained);
}

// The decode that turns a plugin's embedded metadata into these entries is
// logos-module's (ModuleLib::ModuleMetadata) and is unit-tested there. What
// liblogos must keep proving is that a constraint declared in a REAL plugin
// survives the mapping into the gate -- see RealDependencyRangeTest and
// RealMalformedConstraintTest in test_module_manager.cpp.
