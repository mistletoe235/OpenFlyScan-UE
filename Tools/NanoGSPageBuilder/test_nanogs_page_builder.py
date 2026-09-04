from __future__ import annotations

import json
import math
import shutil
import struct
import tempfile
import unittest
from pathlib import Path

import nanogs_page_builder as builder


PROPERTY_NAMES = (
    "x", "y", "z",
    "nx", "ny", "nz",
    "f_dc_0", "f_dc_1", "f_dc_2",
    *(f"f_rest_{index}" for index in range(45)),
    "opacity",
    "scale_0", "scale_1", "scale_2",
    "rot_0", "rot_1", "rot_2", "rot_3",
)


def write_synthetic_ply(path: Path, count: int = 37) -> list[dict[str, object]]:
    source: list[dict[str, object]] = []
    header = [
        "ply",
        "format binary_little_endian 1.0",
        "comment NanoGS Page Builder synthetic SH3 test",
        f"element vertex {count}",
    ]
    header.extend(f"property float {name}" for name in PROPERTY_NAMES)
    header.append("property uchar quality")
    header.append("end_header")
    vertex_struct = struct.Struct("<" + "f" * len(PROPERTY_NAMES) + "B")

    with path.open("wb") as handle:
        handle.write(("\n".join(header) + "\n").encode("ascii"))
        for index in range(count):
            if index == 0:
                position = (0.0, 0.0, 0.0)
                dc = (0.0, 0.0, 0.0)
                opacity = 0.0
                log_scale = (0.0, 0.0, 0.0)
                rotation_wxyz = (2.0, 0.0, 0.0, 0.0)
            else:
                position = (
                    ((index * 17) % 23 - 11) * 0.125,
                    ((index * 7) % 19 - 9) * 0.25,
                    ((index * 13) % 29 - 14) * 0.0625,
                )
                dc = (index * 0.01, -index * 0.005, index * 0.0025)
                opacity = (index - count / 2) * 0.1
                log_scale = (
                    math.log(0.01 + index * 0.0001),
                    math.log(0.02 + index * 0.0002),
                    math.log(0.03 + index * 0.0003),
                )
                rotation_wxyz = (1.0, index * 0.01, -index * 0.005, index * 0.002)
            rest = [((index + 1) * (coefficient + 1) - 20) * 0.0005 for coefficient in range(45)]
            values = (
                *position,
                0.0, 0.0, 0.0,
                *dc,
                *rest,
                opacity,
                *log_scale,
                *rotation_wxyz,
                index & 0xFF,
            )
            handle.write(vertex_struct.pack(*values))
            source.append(
                {
                    "position": position,
                    "dc": dc,
                    "rest": rest,
                    "opacity": opacity,
                    "log_scale": log_scale,
                    "rotation_wxyz": rotation_wxyz,
                }
            )
    return source


def read_payload_by_id(path: Path) -> dict[int, dict[str, object]]:
    _, pages, streams, _, _ = builder.read_container_directories(path)
    result: dict[int, dict[str, object]] = {}
    with path.open("rb") as handle:
        for page in pages:
            by_semantic = {
                item.semantic: item
                for item in streams[page.stream_start:page.stream_start + page.stream_count]
            }
            data: dict[int, bytes] = {}
            for semantic, stream in by_semantic.items():
                handle.seek(stream.file_offset)
                data[semantic] = handle.read(stream.byte_size)
            for index in range(page.splat_count):
                source_id = struct.unpack_from(
                    "<Q", data[builder.SEMANTIC_ORIGINAL_ID], index * 8
                )[0]
                result[source_id] = {
                    "position": struct.unpack_from(
                        "<3f", data[builder.SEMANTIC_POSITION], index * 12
                    ),
                    "rotation": struct.unpack_from(
                        "<4f", data[builder.SEMANTIC_ROTATION], index * 16
                    ),
                    "scale": struct.unpack_from(
                        "<3f", data[builder.SEMANTIC_SCALE], index * 12
                    ),
                    "rgba": tuple(data[builder.SEMANTIC_COLOR_OPACITY][index * 4:index * 4 + 4]),
                    "sh": struct.unpack_from(
                        "<48e", data[builder.SEMANTIC_SH], index * 96
                    ),
                }
    return result


class NanoGSPageBuilderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = Path(tempfile.mkdtemp(prefix="nanogs-page-test-"))
        self.ply = self.temp / "synthetic.ply"
        self.source = write_synthetic_ply(self.ply)
        self.container = self.temp / "synthetic.ngsp"

    def tearDown(self) -> None:
        shutil.rmtree(self.temp)

    def build(self) -> dict[str, object]:
        return builder.build_container(
            self.ply,
            self.container,
            page_points=7,
            bucket_count=4,
            chunk_points=5,
            sort_run_points=3,
            merge_fan_in=2,
        )

    def test_estimate_and_external_build_verify(self) -> None:
        estimate = builder.estimate(self.ply, page_points=7, bucket_count=4)
        self.assertEqual(estimate["source_vertex_count"], len(self.source))
        self.assertEqual(estimate["page_count"], 6)
        self.assertEqual(estimate["raw_payload_bytes"], len(self.source) * 148)

        manifest = self.build()
        self.assertEqual(manifest["directory"]["page_count"], 6)
        self.assertEqual(manifest["build"]["bucket_count"], 4)
        result = builder.verify_container(self.container)
        self.assertEqual(result["splat_count"], len(self.source))
        self.assertEqual(result["unique_source_ids"], len(self.source))
        self.assertEqual(result["crc32"], "ok")
        self.assertEqual(result["sha256"], "ok")

        with Path(str(self.container) + ".json").open("r", encoding="utf-8") as handle:
            disk_manifest = json.load(handle)
        self.assertEqual(
            disk_manifest["coordinate_contract"]["basis"],
            "source PLY XYZ preserved; no axis swap or mirror",
        )

    def test_nanogs_payload_golden_parity(self) -> None:
        self.build()
        payload = read_payload_by_id(self.container)
        self.assertEqual(set(payload), set(range(len(self.source))))

        zero = payload[0]
        self.assertEqual(zero["position"], (0.0, 0.0, 0.0))
        self.assertEqual(zero["rotation"], (0.0, 0.0, 0.0, 1.0))
        self.assertEqual(zero["scale"], (100.0, 100.0, 100.0))
        self.assertEqual(zero["rgba"], (128, 128, 128, 128))
        self.assertEqual(zero["sh"][:3], (0.0, 0.0, 0.0))
        # PLY f_rest is planar (all R, then G, then B); output is coefficient-major RGB.
        self.assertEqual(
            zero["sh"][3:6],
            tuple(struct.unpack("<e", struct.pack("<e", self.source[0]["rest"][offset]))[0]
                  for offset in (0, 15, 30)),
        )

        source = self.source[11]
        converted = payload[11]
        self.assertEqual(
            converted["position"],
            tuple(builder._f32(value * 100.0) for value in source["position"]),
        )
        expected_rotation = builder._normalize_quaternion_wxyz(*source["rotation_wxyz"])
        self.assertEqual(converted["rotation"], expected_rotation)
        expected_scale = tuple(
            builder._f32(builder._exp_f32(value) * 100.0) for value in source["log_scale"]
        )
        self.assertEqual(converted["scale"], expected_scale)
        expected_opacity = builder._sigmoid_f32(source["opacity"])
        self.assertEqual(converted["rgba"], builder._rgba_lane(source["dc"], expected_opacity))

        # Match expf-based sigmoid saturation instead of raising on a large logit.
        self.assertEqual(builder._sigmoid_f32(-1000.0), 0.0)
        self.assertEqual(builder._sigmoid_f32(1000.0), 1.0)

    def test_payload_corruption_is_detected(self) -> None:
        self.build()
        _, _, streams, _, _ = builder.read_container_directories(self.container)
        corrupt = self.temp / "corrupt.ngsp"
        shutil.copyfile(self.container, corrupt)
        with corrupt.open("r+b") as handle:
            handle.seek(streams[0].file_offset)
            byte = handle.read(1)
            handle.seek(streams[0].file_offset)
            handle.write(bytes((byte[0] ^ 0x01,)))
        with self.assertRaisesRegex(builder.VerificationError, "CRC32 mismatch"):
            builder.verify_container(corrupt, verify_sha256=False)

    def test_rejects_non_sh3_source(self) -> None:
        truncated = self.temp / "not-sh3.ply"
        text = (
            "ply\nformat binary_little_endian 1.0\nelement vertex 1\n"
            "property float x\nproperty float y\nproperty float z\nend_header\n"
        ).encode("ascii")
        with truncated.open("wb") as handle:
            handle.write(text)
            handle.write(struct.pack("<3f", 0.0, 0.0, 0.0))
        with self.assertRaisesRegex(builder.FormatError, "full SH3"):
            builder.parse_ply_header(truncated)


if __name__ == "__main__":
    unittest.main()
