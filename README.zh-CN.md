# LampGo ESP32 固件烧录指南

## 先选硬件目标：旧 S3/C6 与新 P4 并存

本仓库包含两套**并存**固件，P4 不是对旧 S3/C6 的覆盖升级。两套镜像、C6 的职责、分区表、接线和烧录命令不能混用。

| 硬件路线 | 固件入口 | C6 的职责 | 应该烧录什么 |
| --- | --- | --- | --- |
| **旧版 S3 + 独立 C6 小屏** | 仓库根目录 + `ESP32_C6_LCD_1_47_UART/` | 小屏，通过 UART 接收 S3 转发的表情 | 本文根目录 `scripts/flash.sh` 对应的 S3，再单独烧录 C6 小屏固件。 |
| **ESP32-P4 头部板 + C6 Wi-Fi** | [`ESP32_P4_HEAD/`](ESP32_P4_HEAD/) | P4 的 ESP-Hosted/SDIO 网络协处理器 | 仅烧录 P4 草图及其自定义分区；**不要**烧录 C6 小屏固件。 |

旧用户继续使用根目录 `XIAO_ESP32S3` 与后端默认的 `motor_transport = "serial"`。P4 用户需要在后端显式设置 `motor_transport = "p4"`；构建、原生 USB、配网与安全步骤请看 [P4 头部板说明](ESP32_P4_HEAD/README.md)。

下文只讲**旧 S3/C6** 的烧录路径。

## License

本固件仓库默认基于 GPL-3.0-only 发布，除非具体文件另有说明。作者、版权和第三方归属见 [LICENSE](LICENSE)、[AUTHORS.md](AUTHORS.md)、[COPYRIGHT](COPYRIGHT) 和 [NOTICE](NOTICE)。

部分摄像头服务和板级支持代码基于 Espressif ESP32 示例代码；相关文件中已有的 Apache-2.0 声明需要保留。发布预编译固件时，应同时提供对应源码、构建脚本、分区表和烧录说明。

## 旧 S3/C6：两个快捷烧录命令

如果你已经安装了 Arduino IDE，或者电脑里有 `arduino-cli`，在源码目录里运行：

```bash
cd YareLampGo_esp32
./scripts/flash.sh --erase --monitor
```

如果你没有 Arduino，可以直接使用仓库里的预编译固件包：

```bash
cd dist/YareLampGo_esp32-firmware
./flash.sh --prebuilt . --erase --monitor
```

`--erase` 会清空旧 WiFi 和旧绑定。第一次烧录、换电脑、配网异常、想重新进入 `Lampgo-Setup-XXXX` 热点时建议保留它。普通升级且想保留原 WiFi 时，可以去掉 `--erase`。

## 应该选哪个命令

- 你拿到的是这个源码仓库，并且电脑装了 Arduino IDE：用第一个命令。
- 你不想安装 Arduino，只想把别人编译好的固件烧进去：用第二个命令。
- 你只是想重新配网、清空旧绑定：两个命令都可以，但要保留 `--erase`。

## 烧录前准备

1. 用 USB 数据线连接 ESP32。注意有些线只能充电，不能传数据。
2. 关闭 Arduino 串口监视器、其它占用串口的软件。
3. 如果电脑接了多个开发板，先列出串口：

```bash
./scripts/flash.sh --list-ports
```

macOS 上常见端口长这样：`/dev/cu.usbmodem2101`、`/dev/cu.usbserial-xxxx`。Linux 上常见端口是 `/dev/ttyACM0`、`/dev/ttyUSB0`。

如果脚本提示有多个串口，需要指定端口：

```bash
./scripts/flash.sh --port /dev/cu.usbmodem2101 --erase --monitor
```

预编译包也一样：

```bash
./flash.sh --prebuilt . --port /dev/cu.usbmodem2101 --erase --monitor
```

## 有 Arduino 的源码烧录

脚本会自动寻找：

- 系统 PATH 里的 `arduino-cli`
- macOS 上 Arduino IDE 自带的 `arduino-cli`
- Arduino ESP32 core 自带的 `esptool`

需要安装 Arduino ESP32 开发板包，目标板是：

```text
XIAO_ESP32S3 + OPI PSRAM
```

常用命令：

```bash
./scripts/flash.sh --erase --monitor
```

只编译、不烧录：

```bash
./scripts/flash.sh --build-only
```

打包一个给非 Arduino 用户使用的预编译固件包：

```bash
./scripts/flash.sh --build-only --package ./dist/YareLampGo_esp32-firmware
```

## 没有 Arduino 的预编译包烧录

预编译包里应该包含这些文件：

- `ESP32_CAMERA.ino.bootloader.bin`
- `ESP32_CAMERA.ino.partitions.bin`
- `boot_app0.bin`
- `ESP32_CAMERA.ino.bin`
- `srmodels.bin`
- `flash.sh`

如果电脑没有 `esptool`，先安装：

```bash
python3 -m pip install --user esptool
```

然后在解压后的固件包目录里烧录：

```bash
./flash.sh --prebuilt . --erase --monitor
```

这条路径不需要安装 Arduino IDE。

## 烧录成功后会看到什么

干净烧录后，串口日志里应该能看到类似内容：

```text
No WiFi credentials stored. Entering provisioning mode.
[provision] WiFi not configured, SoftAP+STA mode
[provision] SSID: Lampgo-Setup-XXXX
```

然后在电脑 WiFi 列表中连接 `Lampgo-Setup-XXXX`，默认密码是 `lampgo123`。连接后回到 LampGo 网页配置家庭 WiFi。

## 当前固件的通话模式

这版固件支持 LampGo 前端的三种通话模式，并额外支持唤醒专用的 `wake_only` profile：

- 稳定模式：ESP32 使用 `stable_raw`；如果配置了唤醒词，LampGo 会自动切到 `wake_only`，只启用 WakeNet，不启用板载 AEC。
- 可打断模式：ESP32 使用 `interruptible_raw`；如果配置了唤醒词，同样会自动切到 `wake_only`，靠 PC 侧文本过滤降低自回声。
- Wake only：ESP32 使用 `wake_only`，启用 ESP-SR WakeNet 单麦唤醒，关闭 AEC，内存压力低于 `aec_experiment`。固件只支持 `Hi,小星`（`wn9_hixiaoxing_tts`）；如果模型分区未烧录这个模型，唤醒监听会保持不可用。
- ESP32 AEC：ESP32 使用 `aec_experiment`，会启用 ESP-SR AFE/AEC，属于实验模式，内存压力更大。

如果你遇到断连、扬声器卡顿、通话不稳定，优先在 LampGo 设置里切回“稳定”模式；需要唤醒词时使用 `wake_only`，不要直接上 `aec_experiment`。

## 上传失败怎么办

如果脚本一直连不上开发板：

1. 按住 ESP32 的 `BOOT` 按钮。
2. 点按一下 `RESET`。
3. 松开 `RESET`。
4. 松开 `BOOT`。
5. 重新运行烧录命令。

如果脚本找不到串口，拔插 USB 后再运行：

```bash
./scripts/flash.sh --list-ports
```

## 配网诊断

如果 LampGo 网页提示 WiFi 扫描失败、probe 失败，先让电脑连接 `Lampgo-Setup-XXXX`，然后在源码目录运行：

```bash
./scripts/provision_diag.sh
```

脚本会把日志写到 `logs/` 目录。运行完后切回正常 WiFi，把日志私下发给开发者排查即可。日志包含本地网络信息，公开贴 issue 前请先检查内容。

如果 LampGo 后端不是默认地址：

```bash
./scripts/provision_diag.sh --lampgo-url http://127.0.0.1:8420
```
