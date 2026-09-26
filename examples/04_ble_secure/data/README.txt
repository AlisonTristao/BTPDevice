Contents of this folder go to the device's LittleFS partition with

    pio run -t uploadfs

Put here (never commit them -- *.key is in .gitignore):

  bally.key   from tools/provision_key.py; imported into NVS on the first boot
              and renamed to bally.key.imported
  wifi.txt    optional, one "ssid,password" per line
