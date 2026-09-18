#include <QObject>

// Sibling of dep_range_fixture_plugin: a real binary whose embedded metadata
// mixes a bare-name dependency with an object-form one whose `version` is a
// NUMBER. A string accessor reads that as absent, silently unconstraining the
// edge — the fail-open the malformedConstraint flag exists to refuse. No
// shipped module declares either, and discovery never instantiates the plugin.
class DepMalformedFixturePlugin : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.logos.test.DepMalformedFixture"
                      FILE "dep_malformed_fixture.json")
};

#include "dep_malformed_fixture_plugin.moc"
