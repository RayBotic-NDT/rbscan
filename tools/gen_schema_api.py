"""Typed helpers for every built-in schema, generated from schemas/*.json.

    python tools/gen_schema_api.py           # writes the files below
    python tools/gen_schema_api.py --check   # fails if they are out of date (CI)

Writes
  include/raybotic/scan/schemas.hpp   C++, header-only: per schema a namespace
                                      with an Attributes struct and, per series,
                                      a Row and a Columns struct with append/read
  python/rbscan/schema_api.py         Python: the same as dataclasses
  tests/schema_api_test.cpp           a write/read round trip of every schema

The schemas stay the source of truth; edit them, not the generated files.
"""

from __future__ import annotations

import argparse
import json
import keyword
import os
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include" / "raybotic" / "scan" / "schemas.hpp"
PY_MODULE = ROOT / "python" / "rbscan" / "schema_api.py"
TEST = ROOT / "tests" / "schema_api_test.cpp"

CPP_SCALAR = {
    "bool": "bool",
    "int8": "std::int8_t",
    "uint8": "std::uint8_t",
    "int16": "std::int16_t",
    "uint16": "std::uint16_t",
    "int32": "std::int32_t",
    "uint32": "std::uint32_t",
    "int64": "std::int64_t",
    "uint64": "std::uint64_t",
    "float32": "float",
    "float64": "double",
    "string": "std::string",
}
PY_SCALAR = {"bool": "bool", "string": "str", "float32": "float", "float64": "float"}
INT_TYPES = {"int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64"}


# ---------------------------------------------------------------------------
# Schemas, flattened like the library does ("extends" merged in)


def load_schemas() -> list[dict]:
    raw = {}
    for path in sorted((ROOT / "schemas").glob("*.json")):
        s = json.loads(path.read_text(encoding="utf-8"))
        raw[f"{s['schema']}/{s.get('version', 1)}"] = s

    def flat(sid: str) -> dict:
        s = raw[sid]
        if "extends" not in s:
            return {**s, "attributes": dict(s.get("attributes", {})),
                    "series": dict(s.get("series", {}))}
        base = flat(s["extends"])
        out = {**base, **{k: v for k, v in s.items() if k not in ("attributes", "series")}}
        out["attributes"] = {**base["attributes"], **s.get("attributes", {})}
        out["series"] = {**base["series"], **s.get("series", {})}
        out.pop("abstract", None)
        if s.get("abstract"):
            out["abstract"] = True
        return out

    out = []
    for sid in raw:
        s = flat(sid)
        if s.get("abstract"):
            continue
        s["id"] = sid
        s["short"] = s["schema"].split(".", 1)[1]
        out.append(s)
    return sorted(out, key=lambda s: s["short"])


CLANG_FORMAT_CANDIDATES = [
    "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clang-format.exe",
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang-format.exe",
    "C:/Program Files/LLVM/bin/clang-format.exe",
]


def clang_format(text: str, name: str) -> str:
    """The repository's style, as the CI lint checks it."""
    exe = os.environ.get("CLANG_FORMAT") or shutil.which("clang-format")
    if not exe:
        exe = next((c for c in CLANG_FORMAT_CANDIDATES if Path(c).exists()), None)
    if not exe:
        sys.exit("clang-format not found (set CLANG_FORMAT)")
    out = subprocess.run([exe, f"--assume-filename={name}"], input=text.encode("utf-8"),
                         capture_output=True, cwd=ROOT, check=True)
    return out.stdout.decode("utf-8").replace("\r\n", "\n")


def check_names(schemas: list[dict]) -> None:
    """Names must work as C++ and Python identifiers."""
    cpp_reserved = {"extra", "time_ns", "size", "row"}
    for s in schemas:
        names = list(s["attributes"]) + [f for ser in s["series"].values()
                                         for f in ser["fields"]]
        for n in names:
            if keyword.iskeyword(n) or n in cpp_reserved or not n.isidentifier():
                sys.exit(f"{s['id']}: '{n}' cannot be a generated member name")


CPP_KEYWORDS = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char",
    "class", "const", "constexpr", "continue", "default", "delete", "do", "double",
    "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend",
    "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
    "not", "nullptr", "operator", "or", "private", "protected", "public", "register",
    "return", "short", "signed", "sizeof", "static", "struct", "switch", "template",
    "this", "throw", "true", "try", "typedef", "typename", "union", "unsigned", "using",
    "virtual", "void", "volatile", "while", "xor",
}


def cpp_name(name: str) -> str:
    """C++ member name: a keyword gets a trailing underscore (static -> static_)."""
    return name + "_" if name in CPP_KEYWORDS else name


def camel(name: str) -> str:
    return "".join(p[:1].upper() + p[1:] for p in name.split("_"))


def dtypes(field: dict) -> list[str]:
    d = field["dtype"]
    return d if isinstance(d, list) else [d]


def fixed_shape(field: dict) -> list[int] | None:
    """Shape if every dimension is a number, else None (resolved per series)."""
    shape = field.get("shape", [])
    return shape if all(isinstance(d, int) for d in shape) else None


def is_array_field(field: dict) -> bool:
    """Stored as an rbscan Array: named dimensions or several allowed dtypes."""
    return fixed_shape(field) is None or len(dtypes(field)) > 1


def comment(text: str, indent: str, width: int = 90) -> list[str]:
    text = " ".join(text.split())
    if not text:
        return []
    return [indent + "/// " + line
            for line in textwrap.wrap(text, width - len(indent) - 4)]


def describe(spec: dict) -> str:
    parts = []
    if spec.get("description"):
        parts.append(spec["description"].rstrip("."))
    if spec.get("units"):
        parts.append(f"[{spec['units']}]")
    if isinstance(spec.get("shape"), list) and spec["shape"]:
        parts.append("shape [" + ", ".join(str(d) for d in spec["shape"]) + "]")
    if "dtype" in spec and len(dtypes(spec)) > 1:
        parts.append("dtype " + " | ".join(dtypes(spec)))
    return ". ".join(p for p in parts if p)


# ---------------------------------------------------------------------------
# C++


def cpp_attr_type(a: dict) -> str:
    t = CPP_SCALAR[a["dtype"]]
    return f"std::vector<{t}>" if a.get("array") else t


def cpp_row_type(f: dict) -> str:
    if is_array_field(f):
        return "Array"
    t = CPP_SCALAR[dtypes(f)[0]]
    shape = fixed_shape(f)
    if not shape:
        return t
    n = 1
    for d in shape:
        n *= d
    return f"std::array<{t}, {n}>"


def cpp_column_type(f: dict) -> str:
    if is_array_field(f):
        return "Array"
    t = cpp_row_type(f)
    return "std::vector<std::uint8_t>" if t == "bool" else f"std::vector<{t}>"


def cpp_elements(f: dict) -> int:
    n = 1
    for d in fixed_shape(f) or []:
        n *= d
    return n


def gen_cpp_schema(s: dict) -> list[str]:
    ns = s["short"]
    L: list[str] = []
    L += comment(f"{s['id']} ({s['category']}): {s.get('description', '')}", "")
    L.append(f"namespace {ns} {{")
    L.append("")
    L.append(f'inline constexpr std::string_view schema_id = "{s["id"]}";')
    L.append(f'inline constexpr std::string_view category = "{s["category"]}";')
    L.append("")
    attrs = s["attributes"]
    L.append("/// The node's attributes; optional ones are std::optional.")
    L.append("struct Attributes {")
    for name, a in attrs.items():
        m = cpp_name(name)
        L += comment(describe(a), "    ")
        t = cpp_attr_type(a)
        if a.get("required"):
            init = "{}" if t != "std::string" and not t.startswith("std::vector") else ""
            L.append(f"    {t} {m}{init};")
        else:
            L.append(f"    std::optional<{t}> {m};")
    L.append("    /// Attributes the schema does not declare (kept on read, written back).")
    L.append("    scan::Attributes extra;")
    L.append("")
    L.append("    [[nodiscard]] scan::Attributes to_attributes() const {")
    L.append("        scan::Attributes out = extra;")
    for name, a in attrs.items():
        m = cpp_name(name)
        if a.get("required"):
            L.append(f'        out["{name}"] = detail::to_value({m});')
        else:
            L.append(f'        if ({m}) out["{name}"] = detail::to_value(*{m});')
    L.append("        return out;")
    L.append("    }")
    L.append("    /// Throws Error for a missing required attribute or a wrong type.")
    L.append("    [[nodiscard]] static Attributes from_attributes(const scan::Attributes& in) {")
    L.append("        Attributes a;")
    L.append("        for (const auto& [key, value] : in) {")
    first = True
    for name, a in attrs.items():
        m = cpp_name(name)
        kw = "if" if first else "} else if"
        first = False
        t = cpp_attr_type(a)
        L.append(f'            {kw} (key == "{name}") {{')
        L.append(f'                a.{m} = detail::from_value<{t}>(value, schema_id, key);')
    if attrs:
        L.append("            } else {")
        L.append("                a.extra.emplace(key, value);")
        L.append("            }")
    else:
        L.append("            a.extra.emplace(key, value);")
    L.append("        }")
    for name, a in attrs.items():
        m = cpp_name(name)
        if a.get("required"):
            L.append(f'        detail::require(in, "{name}", schema_id);')
    L.append("        return a;")
    L.append("    }")
    L.append("};")
    L.append("")
    L.append("/// Adds the node `name` under its category.")
    L.append("inline NodeWriter add(ScanWriter& scan, std::string_view name, const Attributes& a) {")
    L.append("    return scan.add_node(name, schema_id, a.to_attributes());")
    L.append("}")
    L.append("/// The attributes of a node of this schema (any version-1 node).")
    L.append("[[nodiscard]] inline Attributes read(const NodeReader& node) {")
    L.append("    return Attributes::from_attributes(node.attributes());")
    L.append("}")

    for sname, ser in s["series"].items():
        L.append("")
        L += gen_cpp_series(s, sname, ser)
    L.append("")
    L.append(f"}}  // namespace {ns}")
    L.append("")
    return L


def gen_cpp_series(s: dict, sname: str, ser: dict) -> list[str]:
    fields = ser["fields"]
    row = camel(sname) + "Row"
    cols = camel(sname) + "Columns"
    L: list[str] = []
    L += comment(f"Series '{sname}' ({ser.get('storage', 'inline')}): "
                 f"{ser.get('description', '')}", "")
    L.append(f"inline constexpr std::string_view {sname}_series = \"{sname}\";")
    L.append("")
    L.append(f"/// One sample of '{sname}'. Array fields hold their own dtype and shape;")
    L.append("/// optional fields left empty are not written (stored as NaN / 0).")
    L.append(f"struct {row} {{")
    L.append("    std::int64_t time_ns{0};")
    for name, f in fields.items():
        m = cpp_name(name)
        L += comment(describe(f), "    ")
        t = cpp_row_type(f)
        if f.get("optional"):
            L.append(f"    std::optional<{t}> {m};")
        else:
            L.append(f"    {t} {m}{{}};")
    L.append("};")
    L.append("")
    L.append(f"/// Many samples of '{sname}' as columns (bool as 0/1 bytes; array fields")
    L.append("/// one Array [count, sample shape...]). An empty optional column is not written.")
    L.append(f"struct {cols} {{")
    L.append("    std::vector<std::int64_t> time_ns;")
    for name, f in fields.items():
        m = cpp_name(name)
        L.append(f"    {cpp_column_type(f)} {m};")
    L.append("")
    L.append("    [[nodiscard]] std::size_t size() const { return time_ns.size(); }")
    L.append(f"    [[nodiscard]] {row} row(std::size_t i) const {{")
    L.append(f"        {row} r;")
    L.append("        r.time_ns = time_ns.at(i);")
    for name, f in fields.items():
        m = cpp_name(name)
        if is_array_field(f):
            expr = f"detail::sample_of({m}, i)"
            cond = f"!{m}.shape().empty() && {m}.shape()[0] > i"
        else:
            expr = f"{m}[i]" if cpp_row_type(f) != "bool" else f"{m}[i] != 0"
            cond = f"i < {m}.size()"
        if f.get("optional"):
            L.append(f"        if ({cond}) r.{m} = {expr};")
        else:
            L.append(f"        r.{m} = {expr};")
    L.append("        return r;")
    L.append("    }")
    L.append("};")
    L.append("")
    # append one row
    L.append(f"/// Appends one sample to a '{sname}' writer (SeriesWriter or")
    L.append("/// SegmentedSeriesWriter).")
    L.append("template <class Writer>")
    L.append(f"void append(Writer& writer, const {row}& r) {{")
    L.append("    std::vector<FieldValue> f;")
    for name, f in fields.items():
        m = cpp_name(name)
        acc = f"(*r.{m})" if f.get("optional") else f"r.{m}"
        if is_array_field(f):
            v = f"detail::view_of({acc})"
        elif fixed_shape(f):
            v = f"view({acc}.data(), {{{cpp_elements(f)}}})"
        else:
            v = f"scalar({acc})"
        if f.get("optional"):
            L.append(f'    if (r.{m}) f.push_back({{"{name}", {v}}});')
        else:
            L.append(f'    f.push_back({{"{name}", {v}}});')
    L.append("    writer.append(r.time_ns, f);")
    L.append("}")
    L.append("")
    # append columns
    L.append(f"/// Appends every sample of `c` at once (SeriesWriter::append_many).")
    L.append("template <class Writer>")
    L.append(f"void append(Writer& writer, const {cols}& c) {{")
    L.append("    std::vector<FieldValue> f;")
    for name, f in fields.items():
        m = cpp_name(name)
        if is_array_field(f):
            cond = f"!c.{m}.shape().empty()"
            v = f"detail::view_of(c.{m})"
        else:
            cond = f"!c.{m}.empty()"
            base = cpp_row_type(f)
            if base == "bool":
                v = f"ConstArrayView{{DType::Bool, {{c.{m}.size()}}, c.{m}.data()}}"
            elif fixed_shape(f):
                elem = CPP_SCALAR[dtypes(f)[0]]
                v = (f"view(reinterpret_cast<const {elem}*>(c.{m}.data()), "
                     f"{{c.{m}.size() * {cpp_elements(f)}}})")
            else:
                v = f"view(c.{m})"
        if f.get("optional"):
            L.append(f'    if ({cond}) f.push_back({{"{name}", {v}}});')
        else:
            L.append(f'    f.push_back({{"{name}", {v}}});')
    L.append("    writer.append_many(std::span<const std::int64_t>(c.time_ns), f);")
    L.append("}")
    L.append("")
    # read columns
    L.append(f"/// Samples [start, start + count) of '{sname}'; empty when the node has")
    L.append("/// no such series.")
    L.append(f"[[nodiscard]] inline {cols} read_{sname}(const NodeReader& node,")
    L.append(f"{' ' * (len(cols) + 20)}std::uint64_t start = 0,")
    L.append(f"{' ' * (len(cols) + 20)}std::uint64_t count = all) {{")
    L.append(f"    {cols} c;")
    L.append(f'    if (!node.has_series("{sname}")) return c;')
    L.append(f'    const SeriesReader s = node.series("{sname}");')
    L.append("    c.time_ns = s.time(start, count);")
    for name, f in fields.items():
        m = cpp_name(name)
        L.append(f'    {{')
        L.append(f'        Array a = s.read("{name}", start, count);')
        if is_array_field(f):
            L.append(f"        c.{m} = std::move(a);")
        elif cpp_row_type(f) == "bool":
            L.append(f"        c.{m} = detail::bytes_of_bools(a);")
        elif fixed_shape(f):
            L.append(f"        c.{m} = detail::fixed_rows<{cpp_row_type(f)}>(a);")
        else:
            L.append(f"        c.{m} = detail::scalars<{cpp_row_type(f)}>(a);")
        L.append("    }")
    L.append("    return c;")
    L.append("}")
    L.append(f"/// The same as rows.")
    L.append(f"[[nodiscard]] inline std::vector<{row}> read_{sname}_rows(")
    L.append("    const NodeReader& node, std::uint64_t start = 0, std::uint64_t count = all) {")
    L.append(f"    const {cols} c = read_{sname}(node, start, count);")
    L.append(f"    std::vector<{row}> rows;")
    L.append("    rows.reserve(c.size());")
    L.append("    for (std::size_t i = 0; i < c.size(); ++i) rows.push_back(c.row(i));")
    L.append("    return rows;")
    L.append("}")
    return L


CPP_PRELUDE = r"""#pragma once

// GENERATED by tools/gen_schema_api.py from schemas/*.json - do not edit.
//
// Typed helpers for every built-in schema, on top of the generic API: per
// schema a namespace raybotic::scan::schemas::<name> with
//   Attributes                 the node's attributes (optional ones std::optional)
//   add(scan, name, attrs)     adds the node; read(node) reads its attributes
// and per series <s>
//   <S>Row, <S>Columns         one sample, or many as columns
//   append(writer, row|cols)   writes them (cols in one append_many call)
//   read_<s>(node, start, n)   reads columns; read_<s>_rows(...) rows
//
//   namespace sc = raybotic::scan::schemas;
//   sc::xray_source::Attributes src;
//   src.frame_id = "scan_source";
//   src.simulated = true;
//   auto node = sc::xray_source::add(scan, "source_0", src);
//   auto state = node.series(sc::xray_source::state_series);
//   sc::xray_source::append(state, sc::xray_source::StateRow{...});
//
// Header-only; include it where it is used (rbscan.hpp does not).

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "raybotic/scan/reader.hpp"
#include "raybotic/scan/types.hpp"
#include "raybotic/scan/writer.hpp"

namespace raybotic::scan::schemas {

namespace detail {

template <class T>
struct is_vector : std::false_type {};
template <class T>
struct is_vector<std::vector<T>> : std::true_type {};

template <class T>
Value to_value(const T& v) {
    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::string>) {
        return v;
    } else if constexpr (std::is_integral_v<T>) {
        return static_cast<std::int64_t>(v);
    } else if constexpr (std::is_floating_point_v<T>) {
        return static_cast<double>(v);
    } else {
        static_assert(is_vector<T>::value);
        using E = typename T::value_type;
        if constexpr (std::is_same_v<E, std::string>) {
            return v;
        } else if constexpr (std::is_integral_v<E>) {
            return std::vector<std::int64_t>(v.begin(), v.end());
        } else {
            return std::vector<double>(v.begin(), v.end());
        }
    }
}

[[noreturn]] inline void wrong_type(std::string_view schema, std::string_view name) {
    throw Error(std::string(schema) + ": attribute '" + std::string(name) +
                "' has the wrong type");
}

template <class T>
T scalar_from(const Value& v, std::string_view schema, std::string_view name) {
    if constexpr (std::is_same_v<T, std::string>) {
        if (const auto* s = std::get_if<std::string>(&v)) return *s;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (const auto* b = std::get_if<bool>(&v)) return *b;
        if (const auto* i = std::get_if<std::int64_t>(&v)) return *i != 0;
    } else {
        if (const auto* i = std::get_if<std::int64_t>(&v)) return static_cast<T>(*i);
        if (const auto* d = std::get_if<double>(&v)) {
            if constexpr (std::is_floating_point_v<T>) return static_cast<T>(*d);
        }
        if (const auto* b = std::get_if<bool>(&v)) return static_cast<T>(*b);
    }
    wrong_type(schema, name);
}

template <class T>
T from_value(const Value& v, std::string_view schema, std::string_view name) {
    if constexpr (is_vector<T>::value) {
        using E = typename T::value_type;
        T out;
        const bool done = std::visit(
            [&](const auto& x) {
                using X = std::decay_t<decltype(x)>;
                if constexpr (is_vector<X>::value) {
                    using XE = typename X::value_type;
                    if constexpr (std::is_same_v<E, std::string> ==
                                  std::is_same_v<XE, std::string>) {
                        for (const auto& e : x) {
                            if constexpr (std::is_same_v<E, std::string>) {
                                out.push_back(e);
                            } else {
                                out.push_back(static_cast<E>(e));
                            }
                        }
                        return true;
                    } else {
                        return x.empty();  // an empty array has no element type
                    }
                } else {
                    return false;
                }
            },
            v);
        if (!done) wrong_type(schema, name);
        return out;
    } else {
        return scalar_from<T>(v, schema, name);
    }
}

inline void require(const scan::Attributes& in, std::string_view name,
                    std::string_view schema) {
    if (in.find(std::string(name)) == in.end()) {
        throw Error(std::string(schema) + ": required attribute '" + std::string(name) +
                    "' is missing");
    }
}

inline ConstArrayView view_of(const Array& a) { return {a.dtype(), a.shape(), a.data()}; }

/// Sample i of a column Array [count, ...] as an Array of its own.
inline Array sample_of(const Array& column, std::size_t i) {
    Shape shape(column.shape().begin() + 1, column.shape().end());
    Array out(column.dtype(), shape);
    std::memcpy(out.data(), static_cast<const std::byte*>(column.data()) + i * out.byte_size(),
                out.byte_size());
    return out;
}

template <class T>
std::vector<T> scalars(const Array& a) {
    const auto v = a.values<T>();
    return {v.begin(), v.end()};
}

inline std::vector<std::uint8_t> bytes_of_bools(const Array& a) {
    const auto v = a.values<bool>();
    std::vector<std::uint8_t> out(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) out[i] = v[i] ? 1 : 0;
    return out;
}

template <class Row>
std::vector<Row> fixed_rows(const Array& a) {
    using E = typename Row::value_type;
    const auto v = a.values<E>();
    std::vector<Row> out(v.size() / std::tuple_size_v<Row>);
    if (!out.empty()) std::memcpy(out.data(), v.data(), v.size() * sizeof(E));
    return out;
}

}  // namespace detail

"""


def gen_cpp(schemas: list[dict]) -> str:
    L = [CPP_PRELUDE]
    for s in schemas:
        L += gen_cpp_schema(s)
    L.append("}  // namespace raybotic::scan::schemas")
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------
# Python


def py_attr_type(a: dict) -> str:
    t = PY_SCALAR.get(a["dtype"], "int")
    return f"list[{t}]" if a.get("array") else t


def py_doc(text: str, indent: str) -> list[str]:
    text = " ".join(text.split())
    if not text:
        return []
    return [indent + "#: " + line for line in textwrap.wrap(text, 92 - len(indent) - 3)]


def gen_py_schema(s: dict) -> list[str]:
    cls = camel(s["short"])
    attrs = s["attributes"]
    L: list[str] = []
    L.append("@dataclass")
    L.append(f"class {cls}:")
    doc = " ".join(s.get("description", "").split())
    L.append('    """' + f"{s['id']} ({s['category']})." + '\n')
    for line in textwrap.wrap(doc, 84):
        L.append("    " + line)
    L.append('    """')
    L.append("")
    L.append(f'    SCHEMA: ClassVar[str] = "{s["id"]}"')
    L.append(f'    CATEGORY: ClassVar[str] = "{s["category"]}"')
    L.append(f"    SERIES: ClassVar[dict[str, type]] = {{}}")
    req = [(n, a) for n, a in attrs.items() if a.get("required")]
    opt = [(n, a) for n, a in attrs.items() if not a.get("required")]
    for n, a in req:
        L += py_doc(describe(a), "    ")
        L.append(f"    {n}: {py_attr_type(a)}")
    for n, a in opt:
        L += py_doc(describe(a), "    ")
        L.append(f"    {n}: {py_attr_type(a)} | None = None")
    L.append("    #: Attributes the schema does not declare.")
    L.append("    extra: dict[str, Any] = field(default_factory=dict)")
    L.append("")
    L.append("    def to_attributes(self) -> dict[str, Any]:")
    L.append("        return _to_attributes(self)")
    L.append("")
    L.append("    @classmethod")
    L.append(f'    def from_attributes(cls, attributes: dict[str, Any]) -> "{cls}":')
    L.append("        return _from_attributes(cls, attributes)")
    L.append("")
    L.append("    @classmethod")
    L.append(f'    def read(cls, node) -> "{cls}":')
    L.append('        """The attributes of a NodeReader."""')
    L.append("        return _from_attributes(cls, node.attributes)")
    L.append("")
    L.append("    def add(self, scan, name: str):")
    L.append('        """Adds the node `name` to a ScanWriter; returns its NodeWriter."""')
    L.append("        return scan.add_node(name, self.SCHEMA, self.to_attributes())")
    L.append("")
    for sname, ser in s["series"].items():
        L += gen_py_series(s, sname, ser)
    return L


def gen_py_series(s: dict, sname: str, ser: dict) -> list[str]:
    cls = camel(s["short"]) + camel(sname)
    L: list[str] = []
    L.append("")
    L.append("@dataclass")
    L.append(f"class {cls}:")
    doc = " ".join(ser.get("description", "").split())
    L.append('    """' + f"Series '{sname}' of {s['id']} ({ser.get('storage', 'inline')}), "
             "as columns." + "\n")
    for line in textwrap.wrap(doc, 84):
        L.append("    " + line)
    L.append("")
    L.append("    Every column is a numpy array [count, *sample_shape]; optional columns")
    L.append("    left None are not written.")
    L.append('    """')
    L.append("")
    L.append(f'    SCHEMA: ClassVar[str] = "{s["id"]}"')
    L.append(f'    NAME: ClassVar[str] = "{sname}"')
    L.append(f'    STORAGE: ClassVar[str] = "{ser.get("storage", "inline")}"')
    L.append("    #: Time stamps, int64 ns since the Unix epoch.")
    L.append("    time: np.ndarray")
    fields = ser["fields"]
    for n, f in fields.items():
        if not f.get("optional"):
            L += py_doc(describe(f), "    ")
            L.append(f"    {n}: np.ndarray")
    for n, f in fields.items():
        if f.get("optional"):
            L += py_doc(describe(f), "    ")
            L.append(f"    {n}: np.ndarray | None = None")
    L.append("")
    L.append("    def __len__(self) -> int:")
    L.append("        return len(self.time)")
    L.append("")
    L.append("    def append_to(self, writer) -> None:")
    L.append('        """Writes every sample to a SeriesWriter or SegmentedSeriesWriter."""')
    L.append("        _append_columns(self, writer)")
    L.append("")
    L.append("    @classmethod")
    L.append(f'    def read(cls, node, start: int = 0, count: int | None = None) -> "{cls}":')
    L.append('        """Samples [start, start + count) of a NodeReader\'s series (empty')
    L.append('        columns when the node has none)."""')
    L.append("        return _read_columns(cls, node, start, count)")
    L.append("")
    L.append("")
    L.append(f'{camel(s["short"])}.SERIES["{sname}"] = {cls}')
    return L


PY_PRELUDE = '''"""Typed helpers for every built-in schema.

GENERATED by tools/gen_schema_api.py from schemas/*.json - do not edit.

Per schema a dataclass of its attributes (``XraySource``, ...) and per series a
dataclass of columns (``XraySourceState``, ...)::

    from rbscan import schema_api as sc

    with rbscan.ScanWriter.create(path) as scan:
        node = sc.XraySource(frame_id="scan_source", simulated=True).add(scan, "source_0")
        sc.XraySourceState(time=t, emission_state=..., ...).append_to(node.series("state"))

    reader = rbscan.ScanReader(path)
    src = sc.XraySource.read(reader["sensors/source_0"])
    state = sc.XraySourceState.read(reader["sensors/source_0"])   # numpy columns
"""

from __future__ import annotations

import dataclasses
from dataclasses import dataclass, field
from typing import Any, ClassVar

import numpy as np

__all__ = ["BY_SCHEMA"]


def _to_attributes(obj) -> dict[str, Any]:
    out = dict(obj.extra)
    for f in dataclasses.fields(obj):
        if f.name == "extra":
            continue
        value = getattr(obj, f.name)
        if value is not None:
            out[f.name] = value
    return out


def _from_attributes(cls, attributes: dict[str, Any]):
    names = {f.name for f in dataclasses.fields(cls)} - {"extra"}
    known = {k: v for k, v in attributes.items() if k in names}
    extra = {k: v for k, v in attributes.items() if k not in names}
    try:
        return cls(**known, extra=extra)
    except TypeError as e:  # a required attribute is missing
        raise ValueError(f"{cls.SCHEMA}: {e}") from None


def _append_columns(obj, writer) -> None:
    columns = {}
    for f in dataclasses.fields(obj):
        value = getattr(obj, f.name)
        if f.name != "time" and value is not None:
            columns[f.name] = value
    writer.append_many(np.asarray(obj.time, dtype=np.int64), columns)


def _read_columns(cls, node, start: int, count: int | None):
    if cls.NAME not in node.series_names:
        return cls(**{f.name: np.empty(0) for f in dataclasses.fields(cls)
                      if f.default is dataclasses.MISSING})
    series = node[cls.NAME]
    values = {"time": series.time(start, count)}
    for f in dataclasses.fields(cls):
        if f.name != "time":
            values[f.name] = series.read(f.name, start, count)
    return cls(**values)


'''


def gen_py(schemas: list[dict]) -> str:
    L = [PY_PRELUDE]
    for s in schemas:
        L += gen_py_schema(s)
        L += ["", ""]
    L.append("#: Attribute dataclass by schema id, e.g. BY_SCHEMA['raybotic.xray_source/1'].")
    L.append("BY_SCHEMA: dict[str, type] = {")
    for s in schemas:
        L.append(f'    "{s["id"]}": {camel(s["short"])},')
    L.append("}")
    names = [camel(s["short"]) for s in schemas]
    names += [camel(s["short"]) + camel(n) for s in schemas for n in s["series"]]
    L.append("__all__ += [")
    for n in names:
        L.append(f'    "{n}",')
    L.append("]")
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------
# Generated C++ round trip: every attribute and every field, written and read


def test_scalar(dtype: str, i: int) -> str:
    if dtype == "bool":
        return "true" if i % 2 == 0 else "false"
    if dtype == "string":
        return f'std::string("v{i}")'
    if dtype in ("float32", "float64"):
        suffix = "f" if dtype == "float32" else ""
        return f"{i}.25{suffix}"
    return f"{CPP_SCALAR[dtype]}{{{i % 100 + 2}}}"


def gen_test(schemas: list[dict]) -> str:
    L = ["// GENERATED by tools/gen_schema_api.py from schemas/*.json - do not edit.",
         "//",
         "// Every schema's typed helpers: all attributes and all fields written and",
         "// read back unchanged.",
         "",
         "#include <gtest/gtest.h>",
         "",
         "#include <raybotic/scan/rbscan.hpp>",
         "#include <raybotic/scan/schemas.hpp>",
         "",
         '#include "test_util.hpp"',
         "",
         "namespace rs = raybotic::scan;",
         "namespace sc = raybotic::scan::schemas;",
         "using rbscan_test::TempDir;",
         ""]
    for s in schemas:
        ns = s["short"]
        L.append(f"TEST(SchemaApi, {camel(ns)}RoundTrips) {{")
        L.append("    TempDir dir;")
        L.append(f"    sc::{ns}::Attributes a;")
        for i, (n, a) in enumerate(s["attributes"].items()):
            # Named dimensions take 2 (integers, or arrays of two elements).
            if a.get("array"):
                v = "{" + ", ".join(test_scalar(a["dtype"], i + k) for k in range(2)) + "}"
                v = f"{cpp_attr_type(a)}{v}"
            elif a["dtype"] in INT_TYPES:
                v = f"{CPP_SCALAR[a['dtype']]}{{2}}"
            else:
                v = test_scalar(a["dtype"], i)
            L.append(f"    a.{cpp_name(n)} = {v};")
        L.append('    a.extra["custom_note"] = std::string("kept");')
        rows = {}
        L.append("    auto scan = rs::ScanWriter::create(dir.path());")
        L.append(f'        auto written = sc::{ns}::add(scan, "n0", a);')
        for sname, ser in s["series"].items():
            row = camel(sname) + "Row"
            var = f"{sname}_row"
            rows[sname] = var
            L.append(f"        sc::{ns}::{row} {var};")
            L.append(f"        {var}.time_ns = 42;")
            for i, (n, f) in enumerate(ser["fields"].items()):
                d = dtypes(f)[0]
                if is_array_field(f):
                    shape = [2 if isinstance(x, str) else x for x in f.get("shape", [])]
                    count = 1
                    for x in shape:
                        count *= x
                    L.append(f"        {{")
                    L.append(f"            rs::Array arr(rs::DType::{dtype_enum(d)}, "
                             f"{{{', '.join(str(x) for x in shape)}}});")
                    L.append(f"            auto* p = static_cast<{CPP_SCALAR[d]}*>(arr.data());")
                    L.append(f"            for (int k = 0; k < {count}; ++k) p[k] = "
                             f"static_cast<{CPP_SCALAR[d]}>((k + {i}) % 2);")
                    L.append(f"            {var}.{cpp_name(n)} = std::move(arr);")
                    L.append(f"        }}")
                elif fixed_shape(f):
                    vals = ", ".join(test_scalar(d, i + k) for k in range(cpp_elements(f)))
                    L.append(f"        {var}.{cpp_name(n)} = {cpp_row_type(f)}{{{vals}}};")
                else:
                    L.append(f"        {var}.{cpp_name(n)} = {test_scalar(d, i)};")
            dims = sorted({x for f in ser["fields"].values()
                           for x in f.get("shape", []) if isinstance(x, str)})
            dims_arg = "{" + ", ".join(f'{{"{x}", 2}}' for x in dims) + "}"
            if ser.get("storage") == "segmented":
                L.append(f"        auto {sname}_w = written.segmented_series(\"{sname}\", {dims_arg});")
            else:
                L.append(f"        auto {sname}_w = written.series(\"{sname}\", {dims_arg});")
            L.append(f"        sc::{ns}::append({sname}_w, {var});")
            # and the same twice more as columns
            cols = camel(sname) + "Columns"
            L.append(f"        sc::{ns}::{cols} {sname}_cols;")
            L.append(f"        for (int k = 0; k < 2; ++k) {{")
            L.append(f"            {sname}_cols.time_ns.push_back(43 + k);")
            for n, f in ser["fields"].items():
                acc = f"(*{var}.{cpp_name(n)})" if f.get("optional") else f"{var}.{cpp_name(n)}"
                if is_array_field(f):
                    continue
                if cpp_row_type(f) == "bool":
                    L.append(f"            {sname}_cols.{cpp_name(n)}.push_back({acc} ? 1 : 0);")
                else:
                    L.append(f"            {sname}_cols.{cpp_name(n)}.push_back({acc});")
            L.append("        }")
            for n, f in ser["fields"].items():
                if is_array_field(f):
                    acc = f"(*{var}.{cpp_name(n)})" if f.get("optional") else f"{var}.{cpp_name(n)}"
                    L.append(f"        {sname}_cols.{cpp_name(n)} = rbscan_test::stack({acc}, 2);")
            L.append(f"        sc::{ns}::append({sname}_w, {sname}_cols);")
        L.append("        scan.finalize();  // closes the master; the reader opens it below")
        L.append("    const auto reader = rs::ScanReader::open(dir.path());")
        L.append(f'    const auto node = reader.node("{s["category"]}/n0");')
        L.append(f"    const auto b = sc::{ns}::read(node);")
        L.append("    EXPECT_EQ(a.to_attributes(), b.to_attributes());")
        L.append('    EXPECT_EQ(std::get<std::string>(b.extra.at("custom_note")), "kept");')
        for sname, ser in s["series"].items():
            var = rows[sname]
            L.append(f"    {{")
            L.append(f"        const auto rows = sc::{ns}::read_{sname}_rows(node);")
            L.append(f"        ASSERT_EQ(rows.size(), 3u);")
            L.append(f"        EXPECT_EQ(rows[2].time_ns, 44);")
            L.append(f"        for (const auto& r : rows) {{")
            for n, f in ser["fields"].items():
                acc = f"(*{var}.{cpp_name(n)})" if f.get("optional") else f"{var}.{cpp_name(n)}"
                got = f"(*r.{cpp_name(n)})" if f.get("optional") else f"r.{cpp_name(n)}"
                if f.get("optional"):
                    L.append(f"            ASSERT_TRUE(r.{cpp_name(n)}.has_value());")
                if is_array_field(f):
                    L.append(f"            EXPECT_TRUE(rbscan_test::same({got}, {acc}));")
                else:
                    L.append(f"            EXPECT_EQ({got}, {acc});")
            L.append("        }")
            L.append("    }")
        L.append("}")
        L.append("")
    return "\n".join(L)


def dtype_enum(d: str) -> str:
    return {"bool": "Bool", "int8": "Int8", "uint8": "UInt8", "int16": "Int16",
            "uint16": "UInt16", "int32": "Int32", "uint32": "UInt32", "int64": "Int64",
            "uint64": "UInt64", "float32": "Float32", "float64": "Float64"}[d]


# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="only compare; exit 1 if a generated file is out of date")
    args = ap.parse_args()
    schemas = load_schemas()
    check_names(schemas)
    outputs = {HEADER: clang_format(gen_cpp(schemas), "schemas.hpp"),
               PY_MODULE: gen_py(schemas),
               TEST: clang_format(gen_test(schemas), "schema_api_test.cpp")}
    stale = []
    for path, text in outputs.items():
        old = path.read_text(encoding="utf-8") if path.exists() else None
        if old != text:
            stale.append(path)
            if not args.check:
                path.write_bytes(text.encode("utf-8"))
    if args.check and stale:
        for p in stale:
            print(f"out of date: {p.relative_to(ROOT)}", file=sys.stderr)
        print("run: python tools/gen_schema_api.py", file=sys.stderr)
        return 1
    for p in stale:
        print(f"wrote {p.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
