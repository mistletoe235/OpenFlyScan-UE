# NanoGS Spark Tree Builder

第一阶段工具：把单个 3DGS PLY 交给锁定版本的 Spark `build-lod`，生成质量模式
Bhatt-LoD 的 `.rad`/`.radc`，并保存为内容寻址的持久缓存。

```bash
python3 Tools/NanoGSTreeBuilder/nanogs_spark_cache.py plan scene.ply \
  --cache-root Content/NanoGSData/AutoCache

python3 Tools/NanoGSTreeBuilder/nanogs_spark_cache.py build scene.ply \
  --cache-root Content/NanoGSData/AutoCache \
  --method quality --encoding gsplat --max-sh 3
```

缓存 key 包含：

- 完整 PLY SHA256；
- `build-lod` 可执行文件 SHA256；
- 构建算法、编码、SH 阶数和分块选项；
- 本工具格式版本。

默认 builder 路径是 `Tools/NanoGSTreeBuilder/build/build-lod`，该目录被 Git
忽略。也可以设置 `NANOGS_SPARK_BUILDER` 或使用 `--builder`。缓存目录先在同一
文件系统暂存，完成并校验后原子发布；中途失败不会覆盖已有缓存。

当前验证使用 Spark `v2.1.0`、commit
`f22236f95fdd8078f0c12e3aab479523d401daf6` 的 MIT 许可 builder；版本和二进制
revision 记录在 `spark_builder.lock.json`；实际二进制摘要会进入每个内容寻址
缓存的 manifest，二进制本身不进入 Git。项目补丁
`spark_stable_source_ids.patch` 只增加 `--source-id-sidecar`：合并父节点写
`UINT64_MAX`，原始叶节点保存输入 PLY 的 stable ID，不修改 Bhatt-LoD 算法。

这一阶段仍是 Spark RAD 缓存，不是 NanoGS 运行时 Tree v2。后续转换器将读取
该不可变缓存，保留 Exact-HQ 叶子并生成 NanoGS page/tree 目录。

可以独立校验 RAD/RADC 的 chunk 连续性、raw-DEFLATE 属性、父子范围、整棵树，
以及叶节点 stable ID 的唯一性和输入范围：

```bash
python3 Tools/NanoGSTreeBuilder/spark_rad_reader.py \
  Content/NanoGSData/AutoCache/scene/<cache-key>
```

生成 NanoGS Tree v2（内部节点为 Spark 父表示，叶节点从原 PLY Exact-HQ 回填）：

```bash
python3 Tools/NanoGSTreeBuilder/build_nanogs_tree_v2.py build scene.ply \
  Content/NanoGSData/AutoCache/scene/<spark-cache-key> \
  --output-root Content/NanoGSData/TreeV2
```

推荐使用单命令入口。Spark RAD/RADC 保存在 `Saved/NanoGSCache/Spark`，不会被打包；
Tree v2 内容寻址结果发布到 `Content/NanoGSData/TreeV2`，第二次处理同一 PLY 会直接命中：

```bash
python3 Tools/NanoGSTreeBuilder/prepare_nanogs_tree.py /path/to/scene.ply
```

通用 UE Active Scene 还会流式抽样 PLY，估计水平主地面和较空旷的内部出生点。
选点必须满足 `edge_margin_fraction`，不会为了更低的局部障碍评分退化到点云边缘；
找不到合格点时会失败并要求显式调整参数或使用手工坐标。结果缓存在
`Saved/NanoGSCache/Ground`，因此同一 PLY、Transform 和参数不会重复扫描。

可选裁剪默认关闭并且不会覆盖源 PLY。自动模式从均匀样本估计 XY 主方向和稳健
分位边界，随后逐块、逐记录原样复制保留点。结果按源文件摘要和配置写入
`Saved/NanoGSCache/Cropped`，后续 Spark/Tree v2 自动使用该缓存：

```bash
python3 Tools/NanoGSTreeBuilder/prepare_nanogs_tree.py /path/to/scene.ply \
  --auto-crop --crop-tail-quantile 0.001
```

项目不发布任何数据集专用裁剪配置。需要固定裁剪时，请复制自己的 JSON 配置到
仓库外或忽略目录中，并使用 `--crop-config /path/to/config.json`。

裁剪是可选数据清理，不是 LOD，也不保证性能提升。稀疏但有效的远景点可能位于
统计边界外，因此启用前必须检查统计结果和画面对比。

命令完成后，在 UE 内容浏览器导入输出目录中的 `tree.ngstree`，即可创建轻量
`UNanoGSTreeSourceAsset`。资产只保存项目相对目录和校验摘要，运行时继续按需读取
`tree.ngst` 与 `tree_nodes.ngsp`；修改 PLY 或 builder/参数会生成新的内容地址，旧缓存不被覆盖。
