#!/usr/bin/env python3
"""代码引用面门禁：规范见 doc/index.rst「文档规范」。

只认三类可校验事实，其余一律失败：

1. ``doc:<label>`` 必须能在 doc/**.rst 里找到 ``.. _<label>:`` 声明；
   同理 ``:ref:``/``:doc:`` 目标在 RST 侧自检（label 唯一 + doc 文件在册）。
2. 代码面出现的 ``doc/...rst`` 路径必须存在。
3. 代码面不得引用设计内容形态的 ``.md``。白名单只有流程文档（README/
   CONTRIBUTING/SECURITY/CHANGELOG）；跨仓出处必须带仓名标记
   （libca / luiox / 私有仓），带标记即视为 prose，不做存在性校验。
4. RST 里「``X`` 头注」指向微观层：``X`` 必须是某个 ``@defgroup`` 组名或某个
   头文件名。

用法：python doc/check_refs.py [仓库根]
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

CODE_ROOTS = ("src", "test", "tests", "unittest", "unittests", "xmake.lua")
CODE_SUFFIXES = {".hpp", ".cpp", ".h", ".c", ".lua", ".sh", ".py"}

PROCESS_DOCS = {"README.md", "CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md", "AGENTS.md", "dev_plan.md"}
UPSTREAM_MARKERS = ("libca", "luiox", "LL-Java-Zip", "私有仓", "私仓")

DOC_DIRNAME = "doc"
LABEL_DEF = re.compile(r"^\.\. _([\w.-]+):\s*$")
LABEL_REF = re.compile(r"doc:([a-z][a-z0-9]*(?:-[a-z0-9]+)*)")
RST_PATH = re.compile(r"(?:libjhasm/)?doc/[\w./-]+\.rst")
MD_MENTION = re.compile(r"[\w./-]*[\w-]\.md\b")
REF_ROLE = re.compile(r":ref:`([^>`]+)`")
DOC_ROLE = re.compile(r":doc:`([^>`]+)`")
ANON_LINK = re.compile(r"`[^`]*\.(?:md|rst) <([^`>]+\.(?:md|rst))>`_{1,2}")
HEADNOTE = re.compile(r"([A-Za-z_][\w.-]*)(?:\{[\w-]+\})? 头注")
DEFGROUP = re.compile(r"@defgroup\s+([\w.-]+)")


def micro_targets(src_root):
    groups, headers = set(), set()
    if not src_root.is_dir():
        return groups, headers
    for p in src_root.rglob("*.hpp"):
        headers.add(p.name)
        headers.add(p.stem)
        for _, line in iter_lines(p):
            m = DEFGROUP.search(line)
            if m:
                groups.add(m.group(1))
    return groups, headers


def code_files(root):
    for spec in CODE_ROOTS:
        base = root / spec
        if base.is_file():
            yield base
        elif base.is_dir():
            for p in base.rglob("*"):
                if p.is_file() and p.suffix in CODE_SUFFIXES:
                    yield p


def iter_lines(path):
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="replace")
    return enumerate(text.splitlines(), 1)


def collect_labels(doc_dir):
    labels, files, dup = {}, set(), []
    for p in sorted(doc_dir.rglob("*.rst")):
        files.add(p.name)
        for lineno, line in iter_lines(p):
            m = LABEL_DEF.match(line)
            if m:
                label = m.group(1)
                if label in labels:
                    dup.append(f"{p.name}:{lineno}: label 重复定义 {label}（首见 {labels[label]}）")
                labels[label] = f"{p.name}:{lineno}"
    return labels, files, dup


def toctree_entries(index):
    if not index.exists():
        return set()
    inside, entries = False, set()
    for line in index.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith(".. toctree::"):
            inside = True
            continue
        if inside:
            if stripped == "":
                continue
            if stripped.startswith(":"):
                continue
            if not line.startswith("   "):
                inside = False
                continue
            entries.add(stripped.split("#")[0].strip())
    return entries


def check(root):
    problems = []
    doc_dir = root / DOC_DIRNAME
    if not doc_dir.is_dir():
        return [f"{DOC_DIRNAME}/ 不存在：目录已改名或迁移未落地"]
    labels, rst_files, dup = collect_labels(doc_dir)
    problems += dup
    in_tree = toctree_entries(doc_dir / "index.rst")
    groups, headers = micro_targets(root / "src" / "jhasm")

    for p in sorted(doc_dir.rglob("*.rst")):
        if p.name == "index.rst":
            continue
        if p.stem not in in_tree:
            problems.append(f"{p.name}: 未挂进 index.rst 的 toctree（Sphinx -W 会报 orphan）")
    for p in sorted(doc_dir.rglob("*.rst")):
        for lineno, line in iter_lines(p):
            for m in ANON_LINK.finditer(line):
                problems.append(f"{p.name}:{lineno}: 匿名文件链接 {m.group(1)}，改用 :ref:/:doc:")
            for m in MD_MENTION.finditer(line):
                if Path(m.group(0)).name in PROCESS_DOCS:
                    continue
                if any(marker in line for marker in UPSTREAM_MARKERS):
                    continue
                problems.append(f"{p.name}:{lineno}: 文档点名 markdown 规格 {m.group(0)}——设计内容用 :ref:，跨仓出处加仓名标记")
            for m in REF_ROLE.finditer(line):
                target = m.group(1).split("<")[-1].strip().rstrip(">, ")
                if target and target not in labels and not target.startswith("<"):
                    problems.append(f"{p.name}:{lineno}: :ref:`{target}` 无对应 label 声明")
            for m in DOC_ROLE.finditer(line):
                target = m.group(1).split("<")[-1].strip().rstrip(">, ")
                if target and Path(target).name not in rst_files and Path(target).stem not in {f[:-4] for f in rst_files}:
                    problems.append(f"{p.name}:{lineno}: :doc:`{target}` 指向不在册文件")
            for m in HEADNOTE.finditer(line):
                target = m.group(1)
                if target in groups or target in headers:
                    continue
                problems.append(f"{p.name}:{lineno}: 「{target} 头注」在微观层无锚点——应为 @defgroup 组名或头文件名")

    for f in code_files(root):
        rel = f.relative_to(root).as_posix()
        for lineno, line in iter_lines(f):
            for m in LABEL_REF.finditer(line):
                if m.group(1) not in labels:
                    problems.append(f"{rel}:{lineno}: doc:{m.group(1)} 无对应 label 声明")
            for m in RST_PATH.finditer(line):
                rel_path = m.group(0).replace("libjhasm/", "", 1)
                if not (root / rel_path).exists():
                    problems.append(f"{rel}:{lineno}: 文档路径不存在 {m.group(0)}")
            for m in MD_MENTION.finditer(line):
                fname = Path(m.group(0)).name
                if fname in PROCESS_DOCS:
                    continue
                if any(marker in line for marker in UPSTREAM_MARKERS):
                    continue
                problems.append(f"{rel}:{lineno}: 代码面引用 markdown 规格 {m.group(0)}——设计内容转 doc/*.rst，或删指")

    return problems


def main(argv):
    root = Path(argv[1]).resolve() if len(argv) > 1 else Path(__file__).resolve().parents[1]
    problems = check(root)
    if problems:
        print(f"文档引用门禁失败：{len(problems)} 处")
        for p in problems:
            print("  " + p)
        return 1
    print("文档引用门禁通过")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
