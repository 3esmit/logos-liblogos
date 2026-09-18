#include <QObject>

// Exists ONLY for the metadata blob moc bakes in from dep_range_fixture.json:
// an object-form dependency carrying a version range. No shipped module
// declares one, so without a binary that does, the production path
// (discovery -> ModuleLib::extractMetadata -> dependency gate) has no covering
// input. Discovery reads the blob with QPluginLoader::metaData() and never
// instantiates the plugin, so this needs no interface and no behaviour.
class DepRangeFixturePlugin : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.logos.test.DepRangeFixture"
                      FILE "dep_range_fixture.json")
};

#include "dep_range_fixture_plugin.moc"
