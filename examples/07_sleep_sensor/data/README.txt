Contents of this folder go to the device's LittleFS partition with

    pio run -t uploadfs

Put here (never commit them -- *.key and wifi.txt are in .gitignore):

  bally.key   from tools/provision_key.py; imported into NVS on the first boot
              and renamed to bally.key.imported. The TCP link and the
              recovery's uploads need it.
  wifi.txt    one "ssid,password" per line. The application mirrors these
              networks into NVS so the recovery can join the same Wi-Fi.
