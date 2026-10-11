#pragma once
#include <cstdint>
#include <string>

namespace malicompat {

struct DriverArchiveMeta {
    uint32_t schemaVersion{1};
    std::string name{"MaliCompat (Mali-G71)"};
    std::string description{"Vulkan translation shim and descriptor virtualizer for ARM Mali-G71 Bifrost"};
    std::string author{"CodeAbhi826"};
    std::string packageVersion{"1.0"};
    std::string vendor{"MaliCompat"};
    std::string driverVersion{"1.0.0-compat"};
    uint32_t minApi{28};
    std::string libraryName{"libmalicompat.so"};
};

struct DriverJsonConfig {
    std::string name{"MaliCompat-G71"};
    std::string libraryName{"libmalicompat.so"};
    std::string folder{"payload"};
};

class DriverArchivePacker {
public:
    // Generates the standard meta.json definition required by Adrenotools driver loaders
    static std::string GenerateMetaJson(const DriverArchiveMeta& meta);

    // Generates the driver.json file definition
    static std::string GenerateDriverJson(const DriverJsonConfig& config);

    // Generates the libreria linkage identifier file
    static std::string GenerateLibreriaFile(const std::string& libraryName);

    // Creates the default MaliCompat driver package metadata
    static DriverArchiveMeta CreateDefaultMeta();

    // Creates the default driver.json configuration
    static DriverJsonConfig CreateDefaultDriverJson();

    // Packs libmalicompat.so, meta.json, and driver.json into a standard Adrenotools zip package
    static bool CreateDriverBundle(const std::string& outputZipPath, const std::string& soFilePath, const DriverArchiveMeta& meta);
};

} // namespace malicompat
