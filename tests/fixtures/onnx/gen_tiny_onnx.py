#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 tests/fixtures/onnx/ 下的两个**极小合成 ONNX** 固定件（纯标准库，不依赖 onnx 包）。

为什么要有固定件（FACE-ARCWIN-002 / FACE-TEST-005①）：
  `ArcFaceEmbedder` 的 OpenCV DNN 后端在 `initialize()` 里只做 `cv::dnn::readNet(model)`
  —— **不校验节点名、不校验输出维度**。因此「加载成功但前向/维度失败」这条路径，只有
  手里有一个 readNet 能接受、但语义上不满足 512 维契约的模型才能确定性到达。仓库不允许
  提交真实人脸模型（`models/*` 被 gitignore，且模型二进制不入库）⇒ 用自造的极小 ONNX 顶替。

两个固定件的用途（互为对照，**缺一不可**）：
  1. `tiny_identity_1x512.onnx`（147 B，`Identity`，input/output 声明为 `1x512`）
     前向把 `1x3x112x112` 的输入原样透传 ⇒ 输出 37632 个元素 ≠ 512
     ⇒ 命中 `takeFirst512AsF32` 的 `total != 512`（ArcFaceEmbedder.cpp）
     ⇒ **必然返回 nullopt 且写出差异化 err**（用于「自检/embed 失败」臂）。
  2. `tiny_flatten_slice_512.onnx`（260 B，`Flatten(axis=1)` → `Slice([0,512])`）
     输出**恰好** 512 个元素 ⇒ embed 成功
     ⇒ **阳性对照**：证明 cascade / 装配 / readNet 本身可用，从而排除
        「用例因前置缺失而假绿」（`ArcFaceWinRecognizer::initialize` 在 cascade 加载失败时
        **静默** return false、不写 stderr ⇒ 没有阳性对照就分不清「自检失败」与「前置缺失」）。

复现方式（字节确定性，与提交入库的产物逐字节一致；改动本脚本后须重跑并复核 sha256）：
    python tests/fixtures/onnx/gen_tiny_onnx.py
期望 sha256（与提交入库的产物逐字节一致；改脚本后须重跑并复核）：
    tiny_identity_1x512.onnx     a5a23ff8ac4ce112fc80db4a7f251c1584759f37e49efcf2dddb97d48775c8f5
    tiny_flatten_slice_512.onnx  e889b623f52e651f1e8b64927c573f8ed6cb8446a5852ba2fb880ec36aa8cef1

⚠️ 本脚本手写 protobuf 线格式（字段号取自 OpenCV 的 `opencv-onnx.proto` 语义）。
   之所以不引入 `onnx` 包：本机未安装，且入库存一个「必须 pip 安装才能重建」的固定件
   会让 CI/他人无法复现。
"""

import hashlib
import os

# ---- protobuf 线格式最小工具（varint / tag / 长度限定） ----


def vi(n):
    """无符号 varint。"""
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def tg(field, wire):
    return vi((field << 3) | wire)


def fv(field, value):
    """varint 字段。"""
    return tg(field, 0) + vi(value)


def fl(field, payload):
    """长度限定字段。"""
    return tg(field, 2) + vi(len(payload)) + payload


def fs(field, s):
    return fl(field, s.encode())


# ---- ONNX 结构 ----


def tshape(dims):
    # TensorShapeProto{dim=1} / Dimension{dim_value=1}
    return b"".join(fl(1, fv(1, x)) for x in dims)


def tproto(dims):
    # TypeProto{tensor_type=1} / Tensor{elem_type=1(float), shape=2}
    return fl(1, fv(1, 1) + fl(2, tshape(dims)))


def vinfo(name, dims):
    # ValueInfoProto{name=1, type=2}
    return fs(1, name) + fl(2, tproto(dims))


def cint(name, dims, vals):
    # TensorProto{dims=1, data_type=2, int64_data=7, name=8}  —— data_type=7 是 INT64
    p = b"".join(fv(1, d) for d in dims)
    p += fv(2, 7)
    p += fl(7, b"".join(vi(v) for v in vals))
    p += fs(8, name)
    return p


def _node_head(ins, outs, op, name):
    return (
        b"".join(fs(1, i) for i in ins)
        + b"".join(fs(2, o) for o in outs)
        + fs(3, name)
        + fs(4, op)
    )


def node(ins, outs, op, name):
    """NodeProto（无属性）。"""
    return _node_head(ins, outs, op, name)


def attr_i(name, value):
    # AttributeProto{name=1, type=3(INT), i=3} —— 注意 i 的字段号是 3，type 是 20
    return fs(1, name) + fv(3, value) + fv(20, 2)


def node_attrs(ins, outs, op, name, attrs):
    return _node_head(ins, outs, op, name) + b"".join(fl(5, a) for a in attrs)


def graph(initializers, nodes, in_name, in_dims, out_name, out_dims):
    # GraphProto{node=1, name=2, initializer=5, input=11, output=12}
    g = b"".join(fl(1, n) for n in nodes)
    g += fs(2, "tiny_probe_graph")
    g += b"".join(fl(5, i) for i in initializers)
    g += fl(11, vinfo(in_name, in_dims))
    g += fl(12, vinfo(out_name, out_dims))
    return g


def model(graph_proto, ir_version, opset):
    # ModelProto{ir_version=1, producer_name=2, graph=7, opset_import=8}
    m = fv(1, ir_version)
    m += fs(2, "rk3288_opencv_test_fixture")
    m += fl(7, graph_proto)
    m += fl(8, fs(1, "ai.onnx") + fv(2, opset))
    return m


def build_identity():
    """147 B：Identity，input/output 均声明为 1x512（透传实现，故实际输出随输入形状）。"""
    g = graph(
        [],
        [node(["input"], ["output"], "Identity", "n0")],
        "input",
        [1, 512],
        "output",
        [1, 512],
    )
    return model(g, 8, 13)


def build_flatten_slice_512():
    """260 B：Flatten(axis=1) → Slice(starts=[0], ends=[512], axes=[1])，输出恰 512。"""
    nodes = [
        node_attrs(["input"], ["flat"], "Flatten", "n1", [attr_i("axis", 1)]),
        node(["flat", "starts", "ends", "axes"], ["output"], "Slice", "n2"),
    ]
    inits = [
        cint("starts", [1], [0]),
        cint("ends", [1], [512]),
        cint("axes", [1], [1]),
    ]
    g = graph(inits, nodes, "input", [1, 3, 112, 112], "output", [1, 512])
    return model(g, 8, 13)


# 产物名 → (构造函数, 期望 sha256)
FIXTURES = [
    ("tiny_identity_1x512.onnx", build_identity,
     "a5a23ff8ac4ce112fc80db4a7f251c1584759f37e49efcf2dddb97d48775c8f5"),
    ("tiny_flatten_slice_512.onnx", build_flatten_slice_512,
     "e889b623f52e651f1e8b64927c573f8ed6cb8446a5852ba2fb880ec36aa8cef1"),
]


def main():
    out_dir = os.path.dirname(os.path.abspath(__file__))
    ok = True
    for name, build, expected in FIXTURES:
        data = build()
        path = os.path.join(out_dir, name)
        with open(path, "wb") as fh:
            fh.write(data)
        digest = hashlib.sha256(data).hexdigest()
        match = digest == expected
        ok = ok and match
        print("%s  %d B  sha256=%s  %s" % (name, len(data), digest, "OK" if match else "!! 与期望不符"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
