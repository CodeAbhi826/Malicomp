#pragma once
#include <cstdint>
#include <string>

namespace malicompat {

struct DriverArchiveMeta {
    std::string name;
    std::string description;
    std::string author;
    std::string vendor;
    std::string driverVersion;
    std::string libraryName;
    uint32_t minApi{28};
    uint32_t schemaVersion{1};
};

class DriverArchivePacker {
public:
    // Generates the standard meta.json definition required by custom Android driver loaders
    static std::string GenerateMetaJson(const DriverArchiveMeta& meta);

    // Creates the default MaliCompat driver package metadata
    static DriverArchiveMeta CreateDefaultMeta();
};

} // namespace malicompat
