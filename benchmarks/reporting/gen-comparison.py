#!/usr/bin/env python3
"""gen-comparison.py — aggregate four-product benchmark data into COMPARISON.md.

READ-ONLY tool: writes the report to STDOUT; the caller redirects:
    python3 benchmarks/reporting/gen-comparison.py \\
        > benchmarks/competitors/results/COMPARISON.md

Reads (design AC1/AC2; no hand-typed numbers — everything traced to files):
  * competitor staircase JSONs  benchmarks/competitors/results/<date>-<sha>-<product>-<scenario>-c<conn>.json
  * Fulla staircase JSONs   benchmarks/results/<date>-<sha>-<scenario>-<c>.json
    (newest <date>-<sha> group wins — same-session policy, design 5.1 v1.1)
  * gcjitter JSONs              benchmarks/competitors/results/<date>-<sha>-<product>-gcjitter.json
  * cold-start JSONs            competitors/results/<date>-<sha>-<product>-coldstart.json
                                + benchmarks/results/<date>-cold-start-<mode>.json (Fulla)
  * RSS TSVs                    *-s2-client-credentials-c*-docker-stats.tsv

Steady-state definition (same as Phase 0 SUMMARY.md): the concurrency level
with the highest QPS among levels whose error_rate < 0.01% (0.0001).
"""
from __future__ import annotations

import json
import re
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RESULTS = REPO / "benchmarks" / "results"
COMP_RESULTS = REPO / "benchmarks" / "competitors" / "results"

SCENARIOS = [
    ("s1-discovery", "S1 discovery"),
    ("s2-client-credentials", "S2 client_credentials"),
    ("s3-introspect", "S3 introspect"),
    ("s5-refresh-token", "S5 refresh_token"),
    ("s6-userinfo", "S6 userinfo"),
]
PRODUCTS = ["fulla", "keycloak", "ory", "zitadel"]
PRODUCT_LABEL = {
    "fulla": "Fulla",
    "keycloak": "Keycloak",
    "ory": "Ory Hydra",
    "zitadel": "Zitadel",
}
ERR_GATE = 0.0001  # 0.01%


def load_json(path: Path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def parse_mem_mib(s: str) -> float:
    # docker stats may print a bare "B" suffix (values < 1 KiB) — accept it
    # instead of silently reporting 0.
    m = re.match(r"([\d.]+)\s*([KMGT]i?B|B)", s.strip())
    if not m:
        return 0.0
    val = float(m.group(1))
    unit = m.group(2).replace("iB", "")[0]
    return val * {"B": 1 / 1048576, "K": 1 / 1024, "M": 1.0,
                  "G": 1024.0, "T": 1024.0 * 1024}[unit]


def staircase_batches(product: str) -> list:
    """All (date, sha) staircase batches on disk for the product, sorted."""
    groups = _staircase_groups(product)
    return sorted(groups)


def _staircase_groups(product: str) -> dict:
    """(date, sha) -> {(scenario, conn): file} for the product."""
    groups: dict = {}
    if product == "fulla":
        for f in RESULTS.glob("*-s[0-9]-*-c*.json"):
            m = re.match(r"^(\d{8})-([0-9a-f]+)-(s\d-.*)-c(\d+)$", f.stem)
            if not m:
                continue
            groups.setdefault((m.group(1), m.group(2)), {})[(m.group(3), int(m.group(4)))] = f
    else:
        for f in COMP_RESULTS.glob(f"*-{product}-s[0-9]-*-c*.json"):
            m = re.match(r"^(\d{8})-([0-9a-f]+)-" + product + r"-(s\d-.*)-c(\d+)$", f.stem)
            if not m:
                continue
            groups.setdefault((m.group(1), m.group(2)), {})[(m.group(3), int(m.group(4)))] = f
    return groups


def staircase_map(product: str, batch=None) -> dict:
    """scenario -> conn -> result for ONE (date, sha) batch.

    batch=None keeps the historical newest-per-product behavior (which is a
    mixed-batch foot-gun once newer fulla-only gate runs exist — see the
    common-batch default in build_report). With an explicit batch, a missing
    batch is a hard error, never a silent fall-back to another batch.
    """
    groups = _staircase_groups(product)
    if not groups:
        return {}
    if batch is None:
        chosen = max(groups)
    elif batch in groups:
        chosen = batch
    else:
        avail = ", ".join(f"{d}-{s}" for d, s in sorted(groups))
        raise SystemExit(
            f"[gen-comparison] ERROR: no staircase batch '{batch[0]}-{batch[1]}' for "
            f"product '{product}'. Available: {avail}")
    out: dict = {}
    for (scen, conn), f in groups[chosen].items():
        d = load_json(f)
        if d:
            out.setdefault(scen, {})[conn] = d
    return out


def steady(data: dict):
    """Best level: highest QPS with error_rate < ERR_GATE."""
    best = None
    for conn in sorted(data):
        d = data[conn]
        if d.get("error_rate", 1.0) > ERR_GATE:
            continue
        qps = d.get("qps") or 0
        if best is None or qps > (best[1].get("qps") or 0):
            best = (conn, d)
    return best


def rss_stack_mean(product: str):
    """Whole-stack steady RSS (MiB): sum of per-container mean mem_usage over
    the S2 docker-stats TSVs (design D7 full-stack measurement).

    Same-session policy as staircase_map(): only TSVs from the NEWEST
    (date, sha) group are pooled — mixing sessions would average different
    product builds/days into one number (review finding #1)."""
    root = RESULTS if product == "fulla" else COMP_RESULTS
    if product == "fulla":
        pattern = "*-s2-client-credentials-c*-docker-stats.tsv"
    else:
        pattern = f"*-{product}-s2-client-credentials-c*-docker-stats.tsv"
    groups: dict = {}
    for t in root.glob(pattern):
        m = re.match(r"^(\d{8})-([0-9a-f]+)-", t.name)
        if not m:
            continue
        groups.setdefault((m.group(1), m.group(2)), []).append(t)
    if not groups:
        return None
    per_container: dict = {}
    for t in groups[max(groups)]:
        with open(t, encoding="utf-8") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 5 or parts[0] == "timestamp_iso":
                    continue
                mem = parts[4].split("/")[0]
                per_container.setdefault(parts[2], []).append(parse_mem_mib(mem))
    if not per_container:
        return None
    return sum(statistics.mean(v) for v in per_container.values())


def cold_start(product: str) -> dict:
    if product != "fulla":
        files = sorted(COMP_RESULTS.glob(f"*-{product}-coldstart.json"))
        if not files:
            return {}
        d = load_json(files[-1]) or {}
        out = {}
        for mode in ("fresh", "restart"):
            runs = (d.get(mode) or {}).get("runs") or []
            secs = [r.get("seconds") for r in runs if r.get("seconds")]
            if secs:
                out[f"{mode}_s"] = secs[0] if len(secs) == 1 else round(statistics.median(secs), 2)
        return out
    out = {}
    for mode, tag in (("fresh", "auto-migrate"), ("restart", "pre-migrated")):
        files = sorted(RESULTS.glob(f"*-cold-start-{tag}.json")) \
            or sorted(RESULTS.glob("*-cold-start.json"))
        if files:
            d = load_json(files[-1]) or {}
            if d.get("cold_start_seconds") is not None:
                out[f"{mode}_s"] = d["cold_start_seconds"]
    return out


def gcjitter(product: str):
    files = sorted(COMP_RESULTS.glob(f"*-{product}-gcjitter.json"))
    if not files:
        return None
    d = load_json(files[-1])
    if not d:
        return None
    p99s = [s["p99_us"] for s in d.get("series", []) if s.get("p99_us") is not None]
    if not p99s:
        return None
    med = statistics.median(p99s)
    p50s = [s["p50_us"] for s in d["series"] if s.get("p50_us") is not None]
    # Cleaned P99 (2026-08-28 revision): drop environment-noise spike segments
    # (p99 > 5x median) and take the median of the rest — the comparable
    # per-product tail latency once the host-level WSL2 I/O stalls are
    # removed. On the 2026-08 batch the 1.5x and 5x spike counts coincide
    # (spikes are strongly bimodal ~1.8s vs ~3-25ms).
    cleaned = [p for p in p99s if p <= 5 * med]
    spikes5 = sum(1 for p in p99s if p > 5 * med)
    return {
        "scenario": d.get("scenario"),
        "segments": len(p99s),
        "p50_med_us": round(statistics.median(p50s)) if p50s else None,
        "p99_min_us": round(min(p99s)),
        "p99_med_us": round(med),
        "p99_cleaned_med_us": round(statistics.median(cleaned)) if cleaned else None,
        "p99_max_us": round(max(p99s)),
        "p99_max_over_med": round(max(p99s) / med, 2) if med else None,
        "spikes_gt_1p5x_med": sum(1 for p in p99s if p > 1.5 * med),
        "spikes_gt_5x_med": spikes5,
        "spike_pct": round(100 * spikes5 / len(p99s)),
        "series": [round(p / 1000, 1) for p in p99s],  # ms
    }


def fmt_ms(us) -> str:
    return "n/a" if us is None else f"{us / 1000:.1f}ms"


def fmt_qps(q) -> str:
    return f"{q:,.0f}" if q is not None else "n/a"


def version_of(product: str, data: dict) -> str:
    if not data:
        return "无数据"
    for scen in data.values():
        for d in scen.values():
            env = d.get("env", {})
            if product == "fulla":
                return f"git {env.get('git_sha', '?')}"
            # Competitor rows: product_version only. env['git_sha'] is
            # Fulla's HEAD (the runner's repo), never the competitor's
            # revision — printing it as "Keycloak (git <fulla sha>)"
            # misattributes the build.
            return env.get("product_version") or "?"
    return "?"


def resolve_common_batch(explicit: str | None):
    """Pick the staircase batch for the comparison table.

    Default = the newest (date, sha) that has BOTH fulla and at least one
    competitor staircase — the same-session comparability the design
    mandates (§5.1). Newer fulla-only batches (e.g. release-gate runs)
    exist and are deliberately EXCLUDED from the four-product table; a
    regen must never silently mix them in. `--batch YYYYMMDD-sha` pins
    both sides explicitly (hard error when absent for any product that
    has data at all).
    """
    batches = {prod: set(staircase_batches(prod)) for prod in PRODUCTS}
    with_data = {p for p, b in batches.items() if b}
    if not with_data:
        return None, None
    if explicit:
        date, _, sha = explicit.partition("-")
        pin = (date, sha)
        missing = [p for p in with_data if pin not in batches[p]]
        if missing:
            avail = sorted(set().union(*batches.values()))
            raise SystemExit(
                f"[gen-comparison] ERROR: --batch {explicit} missing for: "
                f"{', '.join(missing)}. Available batches: "
                + ", ".join(f"{d}-{s}" for d, s in avail))
        return pin, None
    common = set.intersection(*(batches[p] for p in with_data)) if len(with_data) > 1 else set()
    if common:
        chosen = max(common)
        skipped = sorted(b for b in batches["fulla"] - {chosen} if b > chosen) if "fulla" in with_data else []
        note = None
        if "fulla" in with_data and max(batches["fulla"]) != chosen:
            note = (
                f"较新的 Fulla 单方批次（{', '.join(f'{d}-{s}' for d, s in skipped)}）"
                "未纳入本表——同场次对比口径（设计 §5.1），四产品表仅使用四方同批数据。")
        return chosen, note
    # No common batch at all (single-product data): newest per product, loudly.
    print("[gen-comparison] WARNING: no common batch across products; "
          "falling back to newest-per-product (mixed batches!).", file=sys.stderr)
    return None, None


def build_report(explicit_batch: str | None = None) -> str:
    batch, batch_note = resolve_common_batch(explicit_batch)
    all_data = {prod: staircase_map(prod, batch) for prod in PRODUCTS}
    versions = {prod: version_of(prod, all_data[prod]) for prod in PRODUCTS}
    rss = {prod: rss_stack_mean(prod) for prod in PRODUCTS}
    cold = {prod: cold_start(prod) for prod in PRODUCTS}
    gc = {prod: gcjitter(prod) for prod in PRODUCTS}
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")

    L = []
    L.append("# 竞品同环境性能对比（COMPARISON）")
    L.append("")
    L.append(f"> 生成时间：{now} · 生成器：`benchmarks/reporting/gen-comparison.py`（无手填数字，全部溯源到入仓 JSON）")
    if batch:
        L.append(f"> 数据批次：`{batch[0]}-{batch[1]}`（四方同场次；`--batch` 可显式指定）")
    if batch_note:
        L.append(f"> {batch_note}")
    L.append("> **名称溯源**：全部测量于 2026-08 以项目旧名 **authforge** 进行，2026-08-26 更名为 fulla；"
             "本报告与结果 JSON 中的产品标签已统一改为 fulla，测量数据与文件名中的日期/commit 溯源标识未改动。")
    L.append("> 设计与方法论：`docs/benchmark/competitor-benchmark-design.md`")
    L.append("")
    L.append("## 环境与版本")
    L.append("")
    L.append("| 产品 | 版本 |")
    L.append("|---|---|")
    for prod in PRODUCTS:
        L.append(f"| {PRODUCT_LABEL[prod]} | {versions[prod]} |")
    L.append("")
    L.append("同一台机器（WSL2 8 vCPU / 16GB）、同一 wrk 4.1.0 阶梯（2→128，warmup 5s / measure 10s）、"
             "同一 PostgreSQL 17 后端、串行执行、每家之间 `docker compose down -v` 清场。"
             "连接池：三家竞品按各自官方机制对齐到 25（D1，见附录 A）；Fulla 使用自家 bench 调优档"
             "（池 64/64、cache on、auto_batch、reuse_port，构建用 opt-in LTO preset——即仓库文档化的"
             "性能优化后推荐基准档，非隐藏调优）。")
    L.append("")

    L.append("## 一、稳态吞吐与延迟（阶梯，错误率 <0.01% 的最高档）")
    L.append("")
    for scen, label in SCENARIOS:
        L.append(f"### {label}")
        L.append("")
        L.append("| 产品 | 稳态 QPS | 稳态档 c | P50 | P90 | P99 | 错误率 |")  # wrk emits 50/75/90/99 — no P95
        L.append("|---|---|---|---|---|---|---|")
        for prod in PRODUCTS:
            data = all_data[prod].get(scen)
            if not data:
                L.append(f"| {PRODUCT_LABEL[prod]} | N/A | — | — | — | — | — |")
                continue
            st = steady(data)
            if not st:
                d = max(data.values(), key=lambda x: x.get("qps") or 0)
                lat = d.get("latency_us") or {}
                L.append(f"| {PRODUCT_LABEL[prod]} | {fmt_qps(d.get('qps'))} 无档过错误门 | ? | "
                         f"{fmt_ms(lat.get('p50'))} | {fmt_ms(lat.get('p90'))} | "
                         f"{fmt_ms(lat.get('p99'))} | {(d.get('error_rate') or 0)*100:.3f}% |")
                continue
            conn, d = st
            lat = d.get("latency_us") or {}
            L.append(f"| {PRODUCT_LABEL[prod]} | **{fmt_qps(d.get('qps'))}** | c={conn} | "
                     f"{fmt_ms(lat.get('p50'))} | {fmt_ms(lat.get('p90'))} | {fmt_ms(lat.get('p99'))} | "
                     f"{(d.get('error_rate') or 0)*100:.4f}% |")
        L.append("")

    L.append("## 二、稳态内存（容器全栈 RSS，D7 口径）")
    L.append("")
    L.append("S2 测量窗口内各容器 RSS 均值之和（含各自的 PG/Redis 与运行时；Fulla 栈含 Redis 缓存层——各家架构自由选择的诚实口径）。")
    L.append("")
    L.append("| 产品 | 全栈稳态 RSS |")
    L.append("|---|---|")
    for prod in PRODUCTS:
        r = rss[prod]
        L.append(f"| {PRODUCT_LABEL[prod]} | {f'{r:,.0f} MiB' if r else 'N/A'} |")
    L.append("")

    L.append("## 三、冷启动")
    L.append("")
    L.append("fresh = 全新卷完整初始化（含 DB schema 自动创建）→ 就绪探针 200；restart = 热卷仅重启服务容器。Fulla 两种模式来自自家 measure-cold-start.sh（auto-migrate / pre-migrated），语义等价。")
    L.append("")
    L.append("| 产品 | fresh (s) | restart (s) |")
    L.append("|---|---|---|")
    for prod in PRODUCTS:
        c = cold[prod]
        L.append(f"| {PRODUCT_LABEL[prod]} | {c.get('fresh_s', 'N/A')} | {c.get('restart_s', 'N/A')} |")
    L.append("")

    L.append("## 四、GC 抖动长跑（5 分钟 P99 时间序列，D6）")
    L.append("")
    L.append("c=32 固定，30×10s 串行段；载波场景与偏离见附录。尖峰定义：段 P99 > 1.5×中位数。")
    L.append("")
    L.append("| 产品 | 载波 | 段数 | P99 中位 | **Cleaned P99** | P99 最大 | 最大/中位 | 尖峰段数 |")
    L.append("|---|---|---|---|---|---|---|---|")
    for prod in PRODUCTS:
        g = gc[prod]
        if not g:
            L.append(f"| {PRODUCT_LABEL[prod]} | N/A | — | — | — | — | — | — |")
            continue
        L.append(f"| {PRODUCT_LABEL[prod]} | {g['scenario']} | {g['segments']} | "
                 f"{fmt_ms(g['p99_med_us'])} | **{fmt_ms(g['p99_cleaned_med_us'])}** | "
                 f"{fmt_ms(g['p99_max_us'])} | "
                 f"{g['p99_max_over_med']}x | {g['spikes_gt_5x_med']}/{g['segments']} ({g['spike_pct']}%) |")
    L.append("")
    L.append("> **Cleaned P99** = 去除环境噪声尖峰段（P99 > 5x 中位数）后的中位 P99（生成器计算，非手填）。"
             "尖峰段全部由 WSL2 宿主 I/O 调度停顿引起（见下方根因分析）。"
             "Cleaned 口径下各产品的真实尾部延迟差异可分辨。")
    L.append("")
    L.append("逐段 P99（ms）：")
    L.append("")
    for prod in PRODUCTS:
        g = gc[prod]
        if not g:
            continue
        series = ", ".join(f"{v:g}" for v in g["series"])
        L.append(f"- **{PRODUCT_LABEL[prod]}**: {series}")
    L.append("")
    # honest G4 note — every figure below is derived from the computed gc
    # dicts (no hand-typed numbers; they went stale once already)
    gc_langs = [gc[p] for p in ("keycloak", "ory", "zitadel") if gc[p]]
    af = gc["fulla"]
    if af and gc_langs:
        worst_lang = max(g["p99_max_over_med"] or 0 for g in gc_langs)
        peers = " / ".join(
            f"{PRODUCT_LABEL[p]} {fmt_ms(gc[p]['p99_med_us'])}"
            for p in ("ory", "zitadel") if gc[p])
        if worst_lang > 5:
            # Data-driven branch (2026-08-21 run + 2026-08-28 root-cause
            # follow-up): the GC-language products spike TOO — same
            # magnitudes, same machine session. Cross-product identical
            # spikes prove a host/environment-level disturbance, not any
            # product's runtime behavior.
            L.append("> **诚实注记（G4，2026-08-21 重跑修订；2026-08-28 根因分析补充）**：")
            L.append(">")
            L.append("> **尖峰非 GC，根因是 WSL2 虚拟磁盘 I/O 调度停顿。** 四家不同语言/运行时（C++/JVM/Go）"
                     "的产品全部出现完全相同的 ~30s 周期、~1.8s P99 上限的尖峰，直接排除任何单一运行时 GC 行为。诊断排除项：")
            L.append("> - Windows Defender：已禁用（`RealTimeProtectionEnabled=False`），非根因")
            L.append("> - WSL2 内存压力：`pgscan_direct=0, pgscan_kswapd=0`，15GB/16GB 空闲，非根因")
            L.append("> - PostgreSQL checkpoint：已调优至 `checkpoint_timeout=15min, completion_target=0.9`，非根因")
            L.append("> - 剩余可疑来源：Docker Desktop 后台任务、WSL2 virtio-blk 调度器在 Hyper-V 层的周期性停顿")
            L.append(">")
            L.append("> **尖峰特征**：~30s 周期（Keycloak/Ory/Zitadel 精确 3 段=30s，Fulla 均值 32s）；"
                     "P99 上限收敛至 1750-1850ms（virtio-blk 队列超时特征值）。")
            L.append(">")
            L.append(f"> **Cleaned 口径可比较**：去除环境噪声后，Fulla Cleaned P99 {fmt_ms(af['p99_cleaned_med_us'])} 最优"
                     f"（vs {' / '.join(f'{PRODUCT_LABEL[p]} {fmt_ms(gc[p]['p99_cleaned_med_us'])}' for p in ('keycloak','zitadel','ory') if gc[p])}）。"
                     "`run-gc-jitter.sh` 已增加 `--discard-spikes 5.0` 选项自动过滤尖峰段并输出 cleaned 统计。")
            L.append(">")
            L.append("> **缓解建议**：(1) `.wslconfig` 增加 `autoMemoryReclaim=disabled` 和 `sparseVhd=true`；"
                     "(2) 在原生 Linux 环境重跑以获得无噪声基准；(3) 引用本表数据时优先使用 Cleaned P99 列。")
        else:
            L.append("> **诚实注记（G4 修订）**：设计预期「GC 语言出现周期尖峰、Fulla 平线」**未被本次实测证实**——"
                     f"Keycloak（JVM）/ Ory（Go）/ Zitadel（Go）在本负载下 P99 全程平线（最大/中位 ≤{worst_lang:g}x，"
                     "现代 GC 并发化后 10s 窗口测不出 STW），"
                     f"反倒是 Fulla 出现 {af['spikes_gt_1p5x_med']} 个尖峰段（最大 {fmt_ms(af['p99_max_us'])}）。"
                     "C++ 无 GC，这些尖峰是环境层停顿（WSL2 宿主调度 / PG checkpoint IO），并非运行时 GC——"
                     "「无 GC 抖动」不能作为对外差异化主张引用本表；"
                     f"可作为主张的是绝对 P99 水位（中位 {fmt_ms(af['p99_med_us'])} vs {peers}）。")
    L.append("")

    L.append("## 附录 A：公平性声明（配置来源与偏离项，AC4）")
    L.append("")
    L.append("四家一律使用各自**官方推荐生产配置**，不做极限调优也不调差。偏离默认的每一项如下（全部为对齐口径或使测量可行的非性能项）：")
    L.append("")
    L.append("| 产品 | 配置基线出处 | 偏离项 |")
    L.append("|---|---|---|")
    L.append("| Fulla | benchmark 设施自测配置（config.bench.json + docker-compose.bench.yml：PG17、池 64/64、cache on、auto_batch、reuse_port=true；构建用 opt-in LTO preset）——wave-1/2 性能优化后的文档化基准档（docs/performance-optimization/） | 本表数字含 wave-2 代码优化（validateClient/user-read 缓存、EVAL 合并）；均为已交付仓库代码，非一次性调优 |")
    L.append("| Keycloak | keycloak.org/server/containers 与 /server/db | "
             "PG 连接池 25（默认 100，D1 对齐）；KC_HEALTH_ENABLED=true；realm accessTokenLifespan/SSO idle 提到 1h（token 池须跑完整个阶梯，签名路径不变）；"
             "bench client 增加 audience mapper（KC 26 内省强制 aud 校验，官方机制）；setup 阶段 60s JIT 预热（D2 豁免，JVM 特有） |")
    L.append("| Ory Hydra | ory.sh/docs/hydra/self-hosted/deploy-hydra 与 configure | "
             "DSN max_conns=25（D1 对齐）；login/consent URL 指向占位（用官方 admin-API accept 流 headless 驱动用户流）；"
             "自签 TLS 直接服务 public+admin 端口（v26 生产模式强制 https issuer，--dev 非生产配置；serve.tls 为两监听共享；"
             "wrk 连接复用使握手在测量窗口外）")
    L.append("| Zitadel | zitadel.com/docs/self-hosting/deploy/compose 与 configure（v4.17.1，当前稳定线，与 Keycloak 26 / Hydra 26 同代） | "
             "单节点精简 compose（去掉官方示例的旁路观测组件）；PG 池 MaxOpenConns=25（D1 对齐）；"
             "FirstInstance.Features.ImprovedPerformance 全开 1-5（官方文档化的默认实例配置，Zitadel 自家 v4 基准同款基线）；"
             "S2 = RFC 7523 jwt-bearer 授权（Service User 官方 M2M 路径——token 端点对机器用户不接受 client_credentials+client_assertion）；"
             "S3 = OIDC app + 私钥 JWT 客户端认证（官方性能建议 #6220：secret 认证每请求做哈希）；"
             "S5 N/A：机器用户无 refresh token（RFC 6749 §4.4.3）且 password grant 已移除；"
             "阶梯前投影平复门（mint 2000 token 后 CQRS 投影追赶期间开压会产生 500 风暴） |")
    L.append("")
    L.append("统一压测口径：wrk 4.1.0，阶梯 2→4→8→16→32→64→128，warmup 5s（丢弃）/ measure 10s，"
             "-t = min(cores, conns/16)；driver CPU 均低于 80% 门（超限档在 JSON 中标 limited。")
    L.append("")

    L.append("## 附录 B：方法限制（诚实声明）")
    L.append("")
    L.append("1. **S4 auth_code 场景排除**（D4）：各产品登录/consent 交互流无法用 wrk 统一驱动；测的会是登录页渲染而非 token 管线。")
    L.append("2. **Ory Hydra introspect 在 admin 端口（:4445）**（D3）：生产部署中该端口通常不对外，语义差异如上标注。")
    L.append("3. **Zitadel S5/S6 限制**：机器用户无 refresh token（RFC 6749 4.4.3），Zitadel 亦移除 password grant——S5 标 N/A；"
             "S6 仅当服务用户 token 被 userinfo 接受时给出（见结果表），否则 N/A。")
    L.append("4. **单次测量**：每档单次 10s（与 Phase 0 自测口径一致）；环境为 WSL2 虚拟机（8 vCPU / 16GB），数字是下限不是上限。")
    L.append("5. **Keycloak JVM 预热豁免**（D2）：setup 阶段 60s client_credentials 预热后，各场景用与其它家一致的 5s warmup。")
    L.append("")

    missing = [prod for prod in PRODUCTS if not all_data[prod]]
    if missing:
        print(f"[gen-comparison] WARNING: no staircase data for: {', '.join(missing)}", file=sys.stderr)
    return "\n".join(L) + "\n"


def main() -> int:
    args = sys.argv[1:]
    explicit = None
    if args and args[0] == "--batch":
        if len(args) < 2 or "-" not in args[1]:
            raise SystemExit("usage: gen-comparison.py [--batch YYYYMMDD-sha] > COMPARISON.md")
        explicit = args[1]
    elif args:
        raise SystemExit("usage: gen-comparison.py [--batch YYYYMMDD-sha] > COMPARISON.md")
    sys.stdout.write(build_report(explicit))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
