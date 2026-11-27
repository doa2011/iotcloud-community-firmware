# 2026-11-27 ESP application OTA lease loss under UART overruns

实机 c02854/4c7856：HTTP lease lost，PD12 request3 timeout、低电平、约40秒剩余，ESP RX FIFO 23次、resync18次、MCU RX 0。说明可能是ACK接收丢失，不能断言唯一根因。旧逻辑每4KiB yield但续租ACK等待时仍执行flash，业务同步亦并行。本次加原子ACK pending门闩，发送前关闭，匹配ACK且校验低电平及剩余后开放；HTTP/online worker 在每次erase/write/read及最终commit前等待（最多8s，失败中止，不跳过租约校验），原有一次重试和60s硬件上限保留；准备及释放期间保持快速UART轮询，暂停非必要业务同步，保留media heartbeat。不会因周期低电平报告推断ACK成功。单次已在途flash仍可能和续租重叠，需实机验证；不改MCU或HA。测试覆盖续租一次丢ACK后重试期间worker等待及失败关闭。基于已CI通过的1152fb3配置恢复修复。
