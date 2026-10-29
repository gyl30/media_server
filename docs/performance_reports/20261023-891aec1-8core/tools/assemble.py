#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PARTS = (
    "analysis/summary.md",
    "analysis/s1-2-env.md",
    "analysis/s3-matrix.md",
    "analysis/s4-capacity.md",
    "analysis/s5-differences.md",
    "analysis/s6-metrics.md",
    "analysis/s7-hotspots.md",
    "analysis/s8-stability.md",
    "analysis/section9-tools.md",
    "analysis/s10-13-optimization.md",
    "analysis/appendix-method.md",
    "analysis/defects.md",
)


def main():
    sections = ["# media_server 8 核全协议性能边界与瓶颈分析\n"]
    for part in PARTS:
        text = (ROOT / part).read_text().rstrip() + "\n"
        if part == "analysis/defects.md":
            text = text.replace("# 性能测试中发现的功能问题", "## 附录 B 测试中发现的功能问题", 1).replace("\n## F", "\n### F")
        sections.append(text)
    (ROOT / "report.md").write_text("\n".join(sections))


if __name__ == "__main__":
    main()
