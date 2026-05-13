arduino-cli compile --fqbn 'esp32:esp32:XIAO_ESP32S3:PSRAM=opi' .
arduino-cli upload -p /dev/cu.usbmodem21401 --fqbn 'esp32:esp32:XIAO_ESP32S3:PSRAM=opi' .
arduino-cli monitor -p /dev/cu.usbmodem21401 -c baudrate=115200
