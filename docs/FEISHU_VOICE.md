# 设备直连飞书语音识别（第一阶段）

编程伴侣可以选择两种识别方式：默认的 Mac Apple Speech，或设备通过 Wi-Fi
直接调用飞书流式语音识别。后者不把音频发给 Mac，最终文字仍显示在设备上，
用户确认后才作为编程指令发给 Mac / Codex。当前阶段不包含飞书聊天和个人账号绑定。

## 使用

1. 在飞书开放平台创建并发布企业自建应用，开通 `speech_to_text:speech` 权限。
   官方流式接口文档明确说明**免费版不支持调用**；仅获得 App ID / Secret 不代表
   所在租户具备接口使用资格。
2. 打开新版 Mac 安装器，先安装/修复桥接，再通过“自定义安装应用”更新设备固件。
   四种离线应用组合都包含语音引擎选择能力，默认仍使用 Apple。
3. 用 USB 数据线连接设备，点击“配置飞书语音…”，选择飞书，填写 App ID 和
   App Secret，点击“写入设备”。设备本身也必须已连接可访问飞书的 Wi-Fi。
4. 进入编程伴侣录音。最长 30 秒，短按 OK 结束，识别结果确认后才发送给 Codex；
   识别中可取消，长按 UP 退出会取消当前语音会话。

仅连接一台设备时，Mac 会先检测 USB 通信及固件能力，通过后才打开凭据输入框。
只读握手会自动重试最多三次，并清理上次中断留下的半行数据；保存凭据不会自动
重发，避免在回执丢失时误报结果。也可运行
`python3 tools/feishu_setup.py --port <串口> --check` 单独检测，不读取或写入凭据。

配置成功只表示设备已保存设置，尚未证明云端凭据、权限和租户资格可用；需要录音
测试确认。错误不会自动切换到另一家识别服务，可在 Mac 配置界面显式切回 Apple。
两项凭据同时留空会保留设备原配置；“清除设备凭据”会清除配置中的应用凭据并切回
Apple。App Secret 不保存到 Mac，不出现在命令行或日志，也不编入任何固件。

仅使用语音接口无需个人扫码授权，也不需要聊天读写权限。第一阶段配置的是飞书
中国版企业自建应用，暂不支持 Lark 域名或商店应用凭据。

## 实现与资源边界

- `main/feishu_service.*`：NVS 配置、应用鉴权、HTTPS 流式识别、有限容量录音队列。
  服务不依赖编程伴侣页面，可由其他应用的工作任务调用。
- `main/feishu_protocol.*`：凭据与响应验证。令牌或识别结果过长时拒绝，不静默截断；
  校验响应的 stream_id 和 sequence_id，不把服务端原始错误或凭据回显到 UI。
- `main/coding_bridge.c`：在工作任务中选择识别引擎，接回现有文字确认流程。
- `tools/feishu_setup.py` 与 Mac App：只经 USB 配置，先探测协议再发送凭据，使用
  request_id 匹配回执；凭据通过标准输入传递。LAN 上的同名配置请求不被接受。

录音采用 16 kHz / 16 bit / 单声道，100 ms 一片；4 个 PCM 槽共 12,800 字节，
与录音总时长无关。Base64 直接写入请求缓冲以避免副本；启用 TLS 动态记录缓冲，
握手验证后不保留对端证书。另有响应缓冲和任务栈，日志记录鉴权前、采集开始及
释放后的堆余量和最大连续块，实际数值必须在真机验证。
网络跟不上时结束本次识别并提示重试，不覆盖旧帧或悄悄丢掉语音。

每次识别在开始采集前获取应用令牌，并尽量复用同一主机的 HTTPS 连接，避免首次
TLS 握手阻塞采集。使用 ESP-IDF CA 证书包；禁止重定向。HTTP 单次等待 8 秒，
取消时等待正在进行的请求结束，然后尽力发送中断标记（2 秒超时）。应用退出不
直接删除阻塞中的工作任务；服务等待采集任务退出后才释放队列和缓冲。

共享服务的 `feishu_service_recognize` 是阻塞 API，只能从工作任务调用。
调用者必须保持控制参数和输出缓冲有效到返回；`stop` 请求正常结束，`cancel`
请求丢弃结果。页面和按键回调只能设置标志，不能等待网络。配置写入与识别互斥。

## 开发验证

激活 ESP-IDF 5.5.3 后，运行：

```sh
python3 tools/build_firmware_variants.py
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_*.py'
cc -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -Imain \
  -I"$IDF_PATH/components/json/cJSON" \
  tests/test_feishu_protocol.c main/feishu_protocol.c \
  "$IDF_PATH/components/json/cJSON/cJSON.c" -o /tmp/foloos-feishu-protocol-test
/tmp/foloos-feishu-protocol-test
rm /tmp/foloos-feishu-protocol-test
./mac-installer/build.sh --reuse-firmware
```

最后一条仅在已有当前源码对应的四份固件时使用。可通过 `FOLOOS_MAC_BUILD_DIR`
和 `FOLOOS_RELEASE_DIR` 指定独立打包目录，避免替换仍在运行的旧开发版 App。

主机测试覆盖凭据验证、令牌/识别结果解析、USB 探测与分片回执、旧固件保护、取消
旧 Apple 识别、设备识别期间暂停 Codex 轮询。测试不访问真实飞书，也不写设备。

真机验收尚需：凭据持久化与重启、实际识别中英混合内容、30 秒自动结束、取消和
反复进入退出、断网/错误凭据/权限不足、切回 Apple、运行堆水位和连续多次录音。
构建和主机测试通过不能代替这些项目。

## 接口依据

参考 26 号项目的设备直连思路，应用功能和集成由本项目实现：
https://github.com/SHLcy/ai-passport-feishu

接口参数以飞书官方文档为准：

- https://open.feishu.cn/document/server-docs/authentication-management/access-token/tenant_access_token_internal.md
- https://open.feishu.cn/document/server-docs/ai/speech_to_text-v1/stream_recognize.md

请求中音频为 Base64 PCM；序号从 0 开始，首包 action=1、中间包 action=0、
最后一包 action=2、中断 action=3；引擎为支持中英混合的 `16k_auto`。
