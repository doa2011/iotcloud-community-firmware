# G3 MCU 安装状态收敛修复（2026-11-26）

实机 c3315b / 最后上报 MCU dd6b47：421888/421888，installing，Guard complete 但不可达，最后有效 UART 已过约 37 分钟。源码确认：end_ota 清除 active 后，原先只有 active 传输超时及 pending Guard 请求超时，安装完成但无新启动回报时无限 installing。独立 240 秒安装/重启截止（180 秒 NOR 校验 + 60 秒启动预算），仅新 Guard session、normal 模式、匹配目标 MD5 确认成功；拒绝明确失败，ACK 丢失仍允许等待新启动，禁止安装等待期间再次启动。新增同 session ACK/MD5、超时、新 session held/normal 回归。此修改仅确认状态收敛，不能证明 MCU 实际卡死原因；不自动断电、不绕过 NOR 安全门禁。待实机验证。
