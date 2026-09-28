# 飞书应用凭据配置

Mac App 提供“配置飞书应用…”入口，经 USB 将 App ID / Secret 保存到设备 NVS，
供后续飞书功能使用。当前不调用飞书 API，不包含飞书语音识别或识别方式切换。
编程伴侣固定使用原来的 Mac Apple Speech。

## 使用

1. 使用本版安装器的“自定义安装应用”更新设备固件。四种离线应用组合均支持
   飞书凭据存储，并保留原有 Wi-Fi 设置和已保存的飞书凭据。
2. 用 USB 数据线连接设备，点击“配置飞书应用…”。仅连接一台设备时，App 会先
   验证 USB 和固件能力，再打开凭据输入框。
3. 同时填写 App ID / Secret 并写入设备。两项同时留空会保留已有凭据；首次
   保存时必须填写两项。只有明确点击“清除设备凭据”才清除设备中的凭据。

无需为了存储凭据申请语音识别权限。未来功能需要哪些飞书权限，将随相应功能
说明。保存成功只代表 NVS 写入成功，不代表已经验证应用凭据或取得任何 API 权限。

凭据通过标准输入交给 USB 工具，不出现在命令行、日志和固件镜像中，也不保存在
Mac。只读状态响应只包含“是否已配置”，不会返回 App ID / Secret。

## 旧版兼容

NVS 继续使用 `feishu/settings`，记录保持 200 字节布局。读取时兼容旧版 version=1
记录，不擦除、不重写凭据；原来的语音启用字段被忽略。新版保存时使用 version=2，
保留字段为 0。即使旧版启用了飞书语音，升级本版固件后也只使用 Mac 识别。

USB 状态探测接受 v1/v2 请求并返回 v2 能力，凭据写入只接受 v2。新 Mac App
识别到 v1 固件时会明确要求更新本版固件，并在发送任何凭据之前停止。
这与“USB 未响应”分开处理；超时不能被当作旧版固件的证据。

只读握手最多重试三次，并在探测前用换行恢复串口行边界。保存请求不自动重发，
回执丢失时提示结果尚未确认。配置期间暂停原本运行的桥接，结束后恢复。

## 实现

- `main/feishu_credentials_model.*`：凭据校验、v1/v2 记录兼容和保存准备逻辑。
- `main/feishu_credentials.*`：NVS 读取与写入，不包含网络或音频代码。
- `main/coding_bridge.c`：仅 USB 接受配置，录音或 OTA 期间拒绝写入。
- `tools/feishu_setup.py`：能力探测和配置回执校验。
- `mac-installer/FoloOSCompanion.m`：凭据表单，没有语音引擎选择。

## 验证

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_*.py'
cc -std=c11 -Wall -Wextra -Werror -Imain \
  tests/test_feishu_credentials_model.c main/feishu_credentials_model.c \
  -o /tmp/foloos-feishu-credentials-test
/tmp/foloos-feishu-credentials-test
rm /tmp/foloos-feishu-credentials-test
```

激活 ESP-IDF 5.5.3 后运行 `python3 tools/build_firmware_variants.py`，验证四种组合。
随后运行 `./mac-installer/build.sh --reuse-firmware` 打包已验证的固件。
`FOLOOS_MAC_BUILD_DIR` 和 `FOLOOS_RELEASE_DIR` 可指定独立输出目录。

主机测试覆盖旧版启用/关闭语音时的凭据保留、空输入保留、新凭据替换、显式清除、
损坏记录拒绝、USB 回执和版本探测。实际升级后的凭据保留及 Mac 语音识别仍需
真机确认；开发测试不会自动写设备或刷固件。
