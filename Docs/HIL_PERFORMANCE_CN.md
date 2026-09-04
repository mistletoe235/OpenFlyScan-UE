# OpenFly HIL 最终性能快照

以下数据用于说明当前实现的已验证能力，不是所有设备上的性能承诺。测试场景为
Expo West Filled（12,170,333 splats），Linux/Vulkan，1920×1080 主窗口；发送到
Android 的图像为 1440×1080、JPEG quality 92、30 FPS 上限。

| 指标 | 最终值 |
| --- | ---: |
| UE→Android 完整图像帧率（45 s） | **26.244 FPS**（1,181 帧） |
| Android 正常接收窗口 | **25.9–28.5 FPS** |
| 图像吞吐 | **4.260 MiB/s**（约 35.7 Mbit/s） |
| capture→GPU readback→CPU copy 平均延迟 | **74.131 ms/frame** |
| JPEG encode 平均 | **3.391 ms/frame** |
| TCP `SendAll()` 平均 | **0.685 ms/frame** |
| capture / encode / send failure | **0 / 0 / 0** |
| TCP 重连 | **0** |
| Android dispatch / mailbox coalesced | **0 ms / 0** |
| DJI SimulatorState 源回调 | 约 98–100 Hz |
| Android→UE POSE/摇杆状态（10 s） | **49.798 Hz**（498 个） |
| UE UDP 总入站率 | **149.395 datagrams/s** |
| PING/PONG | **49.798 Hz** |
| UE HEARTBEAT | **4.000 Hz**（250 ms） |
| Android safety/Virtual Stick executor | **25 Hz** |
| peer stale timeout | **1,000 ms** |

图像不是 UDP 视频：UDP 30020/30021 承载 POSE、心跳和安全事件，持久 TCP
30022 承载带 OFFR 帧头的 JPEG。POSE 同时携带位置、姿态、云台和摇杆观测，
因此 UE 侧权威状态的有效频率约为 50 Hz；UE 是 observation-only，25 Hz safety/
Virtual Stick executor 位于 Android。

capture 使用两个独立 RenderTarget、最多两个 in-flight readback 和单槽最新完成帧。
74.131 ms 是重叠流水线单帧延迟，不能当作吞吐周期。非阻塞 TCP 的单次 20 ms
不可写只表示临时背压；连接失效或累计超过 1 秒总 deadline 才关闭连接。
