// Where the device keeps its files: LittleFS on the "storage" partition,
// where `pio run -t uploadfs` puts the data/ folder (bally.key, wifi.txt).
#pragma once
#include <btpdev/config.h>

inline const btpdev::StorageConfig storage_config = {};
