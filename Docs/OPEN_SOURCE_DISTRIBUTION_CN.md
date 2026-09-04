# OpenFlySplat UE 开源交付说明

本仓库同时维护两个交付物，但只提交一份源码历史：

| 交付物 | 放置位置 | 是否含 PLY | 用途 |
| --- | --- | --- | --- |
| UE 5.5 源码工程 | Git 仓库 | 否 | 二次开发、编译、编辑器导入 |
| Linux 已打包程序 | Release 大附件 | 否 | 解压后转换并加载用户自己的 PLY |

大包由仓库同一 revision 的 `Scripts/package_linux.sh` 生成。大包、PLY、Spark
中间缓存和 NanoGS Tree 页面都不进 Git，以免仓库历史永久膨胀。

## 为什么源码工程仍然有少量 Content

`Content/NanoGS/Template/NanoGS_Runtime.umap` 是 UE 启动和 Cook 所需的空地图，
大小约 7 KB，不包含点云、贴图或环境。除此之外项目级 `Content/` 不需要任何
资产。运行时碰撞底板使用 UE 引擎自带 Cube。

`Plugins/AirSim/Content/` 不是默认环境，而是飞行仿真需要的无人机模型、相机、
HUD、传感器材质和碰撞特效。AirSim 的汽车、天气、天气菜单和示例地图均已删除；
`Content/Carla` 和 `Plugins/Carla` 也不进入开源仓库。Cook 从必需的 AirSim
Blueprint 开始，只递归带入这些 Blueprint 实际引用的依赖。

## 两种 PLY 入口

编辑器快速直导不需要脚本：打开 Content Browser，点击 Import，选择二进制
little-endian 3DGS PLY，在 NanoGS 对话框选择 SH 阶数，然后把生成的资产拖进
空地图。这条路径使用 UE 原生资产 Factory，适合小中型数据和快速查看。

多 GB 数据使用分页 Tree 路径：

```bash
export OPENFLYSPLAT_UE_ROOT=/path/to/UnrealEngine-5.5
./Scripts/bootstrap_spark_builder.sh
./Scripts/import_scene.sh /path/to/model.ply
```

它不会把所有点装进单个 UObject，而是生成内容寻址的 Tree/Page，并让运行时按
LOD 和预算加载。18 GB 这类数据应使用此入口。

## 发布大包

```bash
./Scripts/package_linux.sh /path/to/empty/output
```

脚本 Cook 空地图和 AirSim 必需资产，并在程序根目录安装：

- `convert_nanogs_ply.sh`：将任意用户 PLY 转成可替换运行时场景；
- `run_nanogs_profile.sh`：按固定画质/性能配置启动；
- `run_openfly_hil.sh`：按 HIL 配置启动；
- `NanoGSConverter/`：Python 转换器、配置和锁定版本的 `build-lod`；
- `SHA256SUMS`：大包内所有文件摘要。

转换成功后才会原子替换 `Content/NanoGSData/RuntimeActive`，失败不会破坏上一
个可用场景。发布时建议将整个打包目录压缩为 `.tar.zst`，与源码 tag 和
commit SHA 一起上传 Release。

已知限制：保留的 AirSim 上游 `OpticalFlow`/`OpticalFlowVis` 后处理材质与
UE 5.5 Vulkan SM6 不兼容，Cook/启动时会回退到默认材质；本仓库支持的 NanoGS
RGB/HIL 图像路径不使用这两个输出。

## 发布检查

发布源码前运行：

```bash
./Scripts/verify_release_tree.sh
python3 -m unittest discover Tools/NanoGSTreeBuilder -p 'test_*.py'
python3 -m unittest discover Tools/NanoGSPageBuilder -p 'test_*.py'
```

检查器会阻止 PLY、RAD、Tree/Page、大于 100 MiB 的单文件和 CARLA 工程依赖
混入源码交付。第三方代码及资产的许可证见 `THIRD_PARTY_NOTICES.md` 和
`LICENSES/`；正式公开前仍应由项目负责人确认论文代码、商标和素材权属。
