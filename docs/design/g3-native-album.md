# G3 原生相册实验协议

基于已通过 G3 编译的 c5a48b2 分支，不改主分支和备份修复分支。screen.album.set/clear 接收非零 JPEG ID，复用 media submit / ACK / CRC。MCU 媒体状态版本至少 5 才允许执行，旧 MCU 返回 unsupported；HA 需同时检查动作列表和 native_album_supported。仅页六显示，不改 SCN3，不入通知队列，不写图片到 Flash。协议头与 MCU e410fe9 对齐；其他协议仍保留既有上游快照。当前仅实验，尚需联合验证。
