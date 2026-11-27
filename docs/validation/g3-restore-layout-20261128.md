# G3 2026-11-28 恢复布局解码失败

实机 cf51db/cbed45，截图 HTTP400 invalid_backup / layout_decode。原地 JSON 解析、设备配置复制及布局字节分配均已走到后续阶段；旧 scene_decode 内部额外 calloc 整个 scene，失败和格式错误混为一谈。新增 caller-owned disposable candidate API，保留普通 scene_decode 原子语义；恢复直接解码至候选场景，消除重复分配，细分 base64/size/schema/validation。共享协议文件双端同步，仅新增 API，不改线格式或 MCU 运行路径，现有 cbed45 可继续配套。真实 SCN3 布局 encode/Base64/JSON/zero-copy/decode/re-encode 测试覆盖0/1/32提醒和损坏数据拒绝、普通解码不改变原场景。测试侧 Base64 为 fixture codec，并非设备 mbedTLS 本体；仍缺真实用户 PHCB 文件和实机持久化/重启确认，不能断言唯一根因。原 CRC、配置事务、OTA、相册保护不变。
