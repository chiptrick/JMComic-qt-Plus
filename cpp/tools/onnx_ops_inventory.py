"""最小 ONNX 解析器（只为清单统计，不依赖 onnx/ort）。

protobuf wire format：varint(0) / 64bit(1) / len(2) / 32bit(5)。
只读取需要的字段：
  ModelProto.graph(7) -> GraphProto
  GraphProto.node(1) / initializer(5) / input(11) / output(12)
  NodeProto.op_type(4) / attribute(5)
  AttributeProto.name(1) / type(20) / i(3) / f(2) / ints(7)
"""
import sys
import glob
import os
from collections import Counter, defaultdict


def read_varint(b, i):
    shift = 0
    val = 0
    while True:
        c = b[i]
        i += 1
        val |= (c & 0x7F) << shift
        if not c & 0x80:
            return val, i
        shift += 7


def fields(b):
    """yield (field_no, wire_type, payload_or_int, next_index)"""
    i = 0
    n = len(b)
    while i < n:
        key, i = read_varint(b, i)
        fn, wt = key >> 3, key & 7
        if wt == 0:
            v, i = read_varint(b, i)
            yield fn, wt, v
        elif wt == 2:
            ln, i = read_varint(b, i)
            yield fn, wt, b[i:i + ln]
            i += ln
        elif wt == 1:
            yield fn, wt, b[i:i + 8]
            i += 8
        elif wt == 5:
            yield fn, wt, b[i:i + 4]
            i += 4
        else:
            raise ValueError("bad wire type %d" % wt)


def parse_node(b):
    op = ""
    attrs = {}
    for fn, wt, v in fields(b):
        if fn == 4 and wt == 2:
            op = v.decode("utf-8", "replace")
        elif fn == 5 and wt == 2:
            name = ""
            as_int = None
            as_ints = []
            as_str = ""
            for afn, awt, av in fields(v):
                if afn == 1 and awt == 2:
                    name = av.decode("utf-8", "replace")
                elif afn == 3 and awt == 0:      # i: int64
                    as_int = av
                elif afn == 4 and awt == 2:      # s: bytes
                    as_str = av.decode("utf-8", "replace")
                elif afn == 8 and awt == 2:      # ints: packed repeated int64
                    j = 0
                    while j < len(av):
                        x, j = read_varint(av, j)
                        as_ints.append(x)
            if name:
                if as_str:
                    attrs[name] = as_str
                elif as_ints:
                    attrs[name] = as_ints
                elif as_int is not None:
                    attrs[name] = as_int
    return op, attrs


def parse_graph(b):
    nodes = []
    n_init = 0
    for fn, wt, v in fields(b):
        if fn == 1 and wt == 2:
            nodes.append(parse_node(v))
        elif fn == 5 and wt == 2:
            n_init += 1
    return nodes, n_init


def parse_model(path):
    data = open(path, "rb").read()
    for fn, wt, v in fields(data):
        if fn == 7 and wt == 2:
            return parse_graph(v)
    return [], 0


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "python/android/sr_qnn/models"
    files = sorted(glob.glob(os.path.join(root, "*.onnx")))
    global_ops = Counter()
    per_model = {}
    attr_samples = defaultdict(set)
    for f in files:
        nodes, n_init = parse_model(f)
        ops = Counter(op for op, _ in nodes)
        global_ops.update(ops)
        per_model[os.path.basename(f)] = (len(nodes), n_init, ops)
        for op, at in nodes:
            for k, val in at.items():
                key = (op, k)
                if len(attr_samples[key]) < 6:
                    attr_samples[key].add(str(val))

    print("files=%d" % len(files))
    print("global ops:", ", ".join("%s=%d" % (k, v) for k, v in global_ops.most_common()))
    print()
    for name, (n, ni, ops) in per_model.items():
        print("%-46s nodes=%-4d init=%-4d %s" % (name, n, ni,
              ", ".join("%s:%d" % (k, v) for k, v in ops.most_common())))
    print()
    print("attrs:")
    for (op, k), vals in sorted(attr_samples.items()):
        print("  %s.%s = %s" % (op, k, sorted(vals)))


if __name__ == "__main__":
    main()
