// Where the device keeps its files: LittleFS on the "storage" partition,
// where `pio run -t uploadfs` puts the data/ folder (bally.key, wifi.txt).
#pragma once
#include <tracekit/config.h>

inline const tracekit::StorageConfig storage_config = {};
