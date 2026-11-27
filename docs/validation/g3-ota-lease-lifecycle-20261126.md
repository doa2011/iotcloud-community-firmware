# G3 OTA 租约生命周期修复（2026-11-26）

现场：ESP32 上传报 MCU update power lease lost，重启后成功。旧 begin 先发布 wanted 再初始化 ready/failed/deadline，end 后新 begin 可能在 UART owner 清理前复用旧租约。改为 IDLE → PREPARING → REQUESTED → RELEASING → IDLE；仅 UART owner 清理后发布 IDLE。保留真实 PD12 ACK、有效期限及失败终止，不放宽签名校验。新增无间隔重试和准备阶段轮询测试。现场根因仍待实机验证。
