LampGo ESP32 prebuilt firmware package

List ports:
  ./flash.sh --list-ports

Clean flash:
  ./flash.sh --prebuilt . --port /dev/cu.usbmodem2101 --erase --monitor

If you do not have Arduino, install esptool first:
  python3 -m pip install --user esptool
