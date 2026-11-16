"""打印单个 ONNX 的节点序列（op + 主要属性 + 初始化器形状），用于设计 Vulkan kernel。"""
import sys
from onnx_ops_inventory import fields, read_varint, parse_node


def parse_tensor(b):
    dims = []
    name = ""
    dtype = 0
    raw_len = 0
    for fn, wt, v in fields(b):
        if fn == 1 and wt == 2:
            j = 0
            while j < len(v):
                d, j = read_varint(v, j)
                dims.append(d)
        elif fn == 2 and wt == 0:
            dtype = v
        elif fn == 8 and wt == 2:
            name = v.decode("utf-8", "replace")
        elif fn == 9 and wt == 2:
            raw_len = len(v)
    return name, dims, dtype, raw_len


def main(path):
    data = open(path, "rb").read()
    graph = None
    for fn, wt, v in fields(data):
        if fn == 7 and wt == 2:
            graph = v
            break
    nodes, inits = [], {}
    for fn, wt, v in fields(graph):
        if fn == 1 and wt == 2:
            op, at = parse_node(v)
            nodes.append((op, at))
        elif fn == 5 and wt == 2:
            name, dims, dtype, raw = parse_tensor(v)
            inits[name] = (dims, dtype, raw)
    print("%s: %d nodes, %d initializers" % (path, len(nodes), len(inits)))
    for i, (op, at) in enumerate(nodes):
        print("%3d %-18s %s" % (i, op, at if at else ""))
    print("--- initializers (first 12) ---")
    for k, (dims, dtype, raw) in list(inits.items())[:12]:
        print("  %-40s dims=%s dtype=%d raw=%d" % (k, dims, dtype, raw))


if __name__ == "__main__":
    main(sys.argv[1])
