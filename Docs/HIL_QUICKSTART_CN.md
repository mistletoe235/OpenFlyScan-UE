# Android / DJI HIL 使用步骤

## 运行方式

DJI 飞控内置 Simulator 提供运动状态，手机将位姿发送给 UE；UE 在 GS 场景中渲染
虚拟相机，再把图像回传手机。控制和安全动作在 Android 端执行。
`run_expo_east.sh` 默认打开 HIL 窗口并等待手机连接；添加 `--preview` 才启动
不连接硬件的 SimpleFlight 预览。

## 准备

1. Linux 电脑解压带 Expo East 的运行包；此版本无需再次转换 PLY。
2. 手机安装与机型对应的 OpenFly Go：Mini 2 对应 Android MSDK V4，
   Mini 4 Pro 对应 Android MSDK V5。手机 App 单独提供，UE 包不包含 APK；
   自行编译 App 需要自己的 DJI App Key。
3. 无人机与遥控器完成配对，手机通过 USB 连接遥控器，确认 App 能识别飞机。
4. 使用固定台架并拆下螺旋桨。发送模拟起飞或控制前，必须确认手机显示
   **DJI 仿真器已激活**；仅有网络连接不代表仿真器已启动。

## 启动与连接

若已通过 `run_expo_east.sh` 打开窗口，直接进行下一步手机连接，无需重复启动。
否则先关闭无硬件预览窗口，在解压目录的 `Linux/OpenFlySplatUE` 下执行：

```bash
./run_openfly_hil.sh --settings NanoGSConverter/Config/OpenFlyHil.json
```

打开手机的 **UE HIL** 面板，选择一种连接方式：

- **Android 热点直连**：打开手机热点，让 UE 电脑加入热点，然后在 App 启动 HIL。
  通过 UDP 自动发现，不需要固定手机热点网关。
- **已有局域网**：手机和电脑接入同一局域网，填写 **UE 电脑的局域网 IP**，启动 HIL。

默认 UDP 填 **30020**，图像 TCP 填 **30022**。App 启动 HIL 时会尝试启动或接管
DJI Simulator；若未成功，查看 App 的仿真器状态及启动入口，不要在模拟飞机飞行中
强制重启仿真器。在 HIL 相机来源中选择 **UE**，显示仿真画面而不是实际相机画面。

开始操纵前，依次确认：**peer 心跳在线 → DJI 仿真器激活且位姿持续更新 → TCP
图像已连接且帧数增长**。随后用实体遥控器操纵模拟飞机。结束时先让模拟飞机降落，
再在手机停止仿真/HIL，最后关闭 UE。

## 端口与常见问题

| 接收端 | 端口 | 用途 |
| --- | --- | --- |
| UE 电脑 | UDP 30020 | 手机发送 HELLO、POSE、心跳和 PING |
| Android 手机 | UDP 30021 | UE 回复心跳、PONG 和安全事件 |
| Android 手机 | TCP 30022 | 手机监听；UE 主动连接手机并推送 JPEG 图像 |

AirSim RPC 的 41451 是另一条接口，手机连接不依赖 ADB。仅在可信局域网中使用，
当前协议不提供认证或加密，不要把端口暴露到公网。

| 现象 | 排查 |
| --- | --- |
| 等待手机连接 | 同一网络、UE IP、UDP 端口、防火墙及热点隔离。 |
| 图像在线但等待 DJI Simulator | 网络已通，检查飞机/遥控器连接和手机仿真器状态。 |
| 位姿在线但没有图像 | 检查手机 TCP 30022 监听、防火墙、相机来源是否选择 UE。 |
| 端口占用 | 先关闭无硬件预览或另一份 HIL 程序。 |
| 只看到天空 | 检查 `Content/NanoGSData/RuntimeActive/active_scene.runtime.json` 及其引用的树资源；转换后需重启 UE。 |
| 起点卡在建筑里 | 先退出 UE，调整起点/地面配置后重新生成，不要靠硬件控制试探脱困。 |

转换器生成的 PlayerStart 是 AirSim/HIL 共用起点，起点高度相对估计地面。
默认隐藏地板不等于建筑碰撞模型，GS 可见表面不会自动变成碰撞几何。
默认图像配置为 1440×1080、JPEG 质量 92、最高 30 FPS；V4/V5 的状态 API 不同，
手机位姿发送频率不等于新鲜飞控状态的实际更新频率。
