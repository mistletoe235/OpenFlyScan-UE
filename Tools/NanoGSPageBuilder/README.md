# NanoGS Page Builder v1

这是一个只依赖 Python 3 标准库的离线转换器原型。它把标准
`binary_little_endian 1.0`、完整 SH3 的 3DGS PLY 转成可随机读取的
NanoGS Page 容器。目标是让 50M/100M+ 数据集的 CPU 内存占用只与输入块、
一个外排 run 和一个输出 page 有关，而不再构造全量 `FGaussianSplatData`。

这里的“无损”指相对当前 NanoGS HQ 运行时契约：保留每个源 splat，位置、
归一化四元数和线性 scale 都是 Float32；颜色/opacity 与当前 NanoGS 一样走
`FFloat16Color` 往返后量化到 RGBA8；DC + 15 个高阶系数与当前 NanoGS 一样
存成 48 个 Float16。它不是把源 PLY 的所有 Float32 SH 原样保留。

## 快速使用

只检查 header 并估算磁盘空间，不产生文件：

```bash
python3 Tools/NanoGSPageBuilder/nanogs_page_builder.py estimate scene.ply
python3 Tools/NanoGSPageBuilder/nanogs_page_builder.py build scene.ply scene.ngsp --dry-run
```

构建和完整校验：

```bash
python3 Tools/NanoGSPageBuilder/nanogs_page_builder.py build scene.ply scene.ngsp
python3 Tools/NanoGSPageBuilder/nanogs_page_builder.py verify scene.ngsp
```

默认每页 65,536 点。`build` 会生成 `scene.ngsp` 和
`scene.ngsp.json`；后者记录源文件 SHA-256、容器 SHA-256、坐标契约、每页
边界以及每条 stream 的 offset/count/CRC32。临时文件默认放在输出目录，
可用 `--temp-dir` 指到空间更充足、最好与输出同盘的目录。

大文件常用参数：

```bash
python3 Tools/NanoGSPageBuilder/nanogs_page_builder.py build \
  scene.ply scene.ngsp \
  --page-points 65536 \
  --chunk-points 32768 \
  --sort-run-points 262144 \
  --merge-fan-in 64 \
  --temp-dir /fast-scratch
```

- 自动 bucket 数是 2 的幂，目标为平均每 bucket 八页，上限 4096。
- 即使所有点落入一个 bucket，也会通过有界 sorted runs 和多轮 k-way merge
  处理，不会退化为全量内存排序。
- 临时 spool 是 156 B/splat；最终有效 payload 是 148 B/splat，另有少量
  64B 对齐和目录开销。100M 点分别约为 14.53 GiB 和 13.78 GiB。
- 构建末尾默认再顺序读取一次容器计算 SHA-256；若交付流程另行做 hash，
  可用 `--no-file-sha256` 省掉这次 I/O，但 CRC32 仍总是写入和校验。

## 两阶段外存算法

1. 第一遍只读位置，计算源 XYZ 在 NanoGS 厘米空间的中心 bounds，同时计算
   源文件 SHA-256。
2. 第二遍按块解码，每个点生成 63-bit Morton key，写入高位空间 bucket。
3. 每个 bucket 按 `--sort-run-points` 产生内存有界的有序 run；run 数超过
   `--merge-fan-in` 时做多轮归并。
4. 按 Morton 顺序逐页转置为 SoA stream，写 payload、CRC32、紧致 2σ
   rotated-Gaussian AABB，最后回填 header 和目录。

任何阶段都不会保留全量解码 payload。完整 `verify` 只额外使用
`total_splats / 8` 字节的 ID bitset（100M 点约 11.9 MiB），并一次只读取一页
位置/旋转/scale 来复算 bounds。

## 坐标与量化契约

- position：源 PLY XYZ 原样保留，只乘 100，从米变成 UE 厘米。
- rotation：源 `rot_0..3 = WXYZ`，归一化并输出 `XYZW`；不换轴。
- scale：对源 log-scale 做 `exp`，再乘 100，输出厘米。
- opacity：sigmoid 后先做 Float16 往返，再 clamp/round 到 uint8。
- RGB：`0.5 + SH_C0 * f_dc`，先做 Float16 往返，再 clamp/round 到 uint8。
- SH：DC 先存，随后 15 个系数；每个系数按 RGB 排列，全部 Float16。
- `OriginalId`：源 vertex 的零基序号，uint64，输出中构成 `[0,N)` 的排列。
- 不在文件中烘焙 X 镜像。LCC 兼容需要的镜像由 Actor transform 或 runtime
  metadata 显式执行。
- page support bounds 是每个归一化旋转 Gaussian 的 2σ ellipsoid AABB 的
  union，不只是中心点 bounds。

## 固定小端 wire format

禁止 runtime 直接 `memcpy` 未显式 pack 的 C++ struct；应逐字段按以下 offset
读取。所有 offset/count/size 都是 uint64。

Header 固定 256 B：

| Offset | 字段 | 类型 |
|---:|---|---|
| 0 | Magic `NGSPAGE1` | 8 bytes |
| 8,12,16,20 | Version, EndianTag, PayloadFormat, Flags | 4×u32 |
| 24..104 | HeaderBytes, FileBytes, PageCount, PageDirectoryOffset, PageRecordBytes, StreamCount, StreamDirectoryOffset, StreamRecordBytes, PayloadOffset, PayloadBytes, TotalSplatCount | 11×u64 |
| 112,136 | Scene support bounds min/max | 6×f64 |
| 160,168 | SupportSigma=2, UnitsToCentimeters=1 | 2×f64 |
| 176,180,184 | CoordinateSystem=1, SHOrder=3, SHLayout=1 | 3×u32 |
| 188,192 | DirectoryCRC32, HeaderCRC32 | 2×u32 |
| 196..255 | reserved zero | bytes |

Page record 固定 160 B：5×u64 的 page/index/count/stream range；中心 bounds
和 support bounds 共 12×f64；SupportSigma 与 MaxScaleCm 共 2×f64；Flags 和
Reserved 为 2×u32。`Flags bit0` 表示 rotated-Gaussian 2σ AABB。

Stream record 固定 80 B：`PageId u64`；`Semantic/Encoding u32`；
`ElementCount/ComponentCount/ElementStrideBytes/FileOffset/ByteSize/
UncompressedByteSize` 六个 u64；`PayloadCRC32/Flags u32`；`Reserved u64`。
v1 的 stream flags 和 reserved 固定为零。

每页恰好六条 stream：

| Semantic | Encoding | 数据 | Stride |
|---:|---:|---|---:|
| 1 | 1 | Position Float32×3 | 12 |
| 2 | 1 | Rotation Float32×4 XYZW | 16 |
| 3 | 1 | Scale Float32×3 cm | 12 |
| 4 | 2 | Color/Opacity UNorm8 RGBA | 4 |
| 5 | 3 | SH3 Float16×48，DC first | 96 |
| 6 | 4 | Original source ID uint64 | 8 |

## 测试

```bash
python3 -m unittest discover -s Tools/NanoGSPageBuilder -p 'test_*.py' -v
```

测试会生成带额外属性的小型 SH3 binary PLY，以 3 点/run、2-way merge
强制走多轮外排，检查 NanoGS 的位置/旋转/scale/RGBA/SH golden parity，执行
完整 verify，并确认 payload 位翻转会被 CRC 检出。
