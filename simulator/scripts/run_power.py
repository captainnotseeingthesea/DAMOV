#!/usr/bin/env python3
import os
import subprocess
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path
from typing import List, Tuple

# 根目录
ROOT = Path("/home/xuanyi/research/simulator")

# 模拟器路径
SIMULATOR_ROOT = ROOT / "DAMOV" / "simulator"
STATS_FOLDER = SIMULATOR_ROOT / "zsim_stats"
OUTPUT_FOLDER = SIMULATOR_ROOT / "zsim_stats"

# MCPAT路径
MCPAT_SCRIPT = ROOT / "mcpat" / "scripts" / "mcpat.py"

# 并发配置
MAX_WORKERS = 10

# 实验配置
EXPERIMENT_CONFIG = {
    "datasets": [
        "roadNet-CA-ungraph",
        "delaunay-n21",
        "venturiLevel3",
        "hugetrace-00000",
        "apache2",
        "ecology1"
    ],
    "applications": ["SSSP", "SSWP", "BFS", "CC"],
    "batchSizes": [100000],
    "deleteRatios": [0.1],
    "number_of_cores": [16],
    "type": "ooo_nuca",
}

# 框架 + 硬件配置
FRAMEWORK_TASKS = [
    # ("kickstarter", "base"),
    # ("kickstarter", "L1_64"),
    # ("kickstarter", "stride_prefetch"),
    # ("kickstarter", "ampm_prefetch"),
    # ("kickstarter", "bop_prefetch"),
    # ("kickstarter", "imp_prefetch"),
    # ("kickstarter", "graph_prefetch_4_32_2"),
    # ("kickstarter", "graph_prefetch_with_filter_4_32_2"),
    # ("wsgraph", "base"),
    # ("wsgraph", "L1_64"),
    # ("wsgraph", "stride_prefetch"),
    # ("wsgraph", "ampm_prefetch"),
    # ("wsgraph", "bop_prefetch"),
    # ("wsgraph", "imp_prefetch"),
    ("wsgraph", "graph_prefetch_4_32_2"),
    # ("wsgraph", "graph_prefetch_with_filter_4_32_2"),
]


def execute_power_collection(stats_dir: Path, output_dir: Path) -> Tuple[Path, int, float]:
    """执行单个功耗收集任务"""
    start_time = time.time()
    output_dir.mkdir(parents=True, exist_ok=True)

    log_file = output_dir / "mcpat.log"
    cmd = [
        "python3", str(MCPAT_SCRIPT),
        "-z", str(stats_dir),
        "-a", "result",        # 固定 app 名称
        "-d", str(output_dir)
    ]

    print(f"Executing: {' '.join(cmd)} -> {log_file}")

    try:
        with open(log_file, "w") as f:
            return_code = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT)
        exec_time = time.time() - start_time

        with open(log_file, "a") as f:
            f.write(f"\nExecution time: {exec_time:.2f} seconds\n")

        if return_code != 0:
            print(f"Error: {stats_dir} failed with return code {return_code}")

        return stats_dir, return_code, exec_time

    except Exception as e:
        print(f"Exception while running {stats_dir}: {e}")
        return stats_dir, -1, 0.0


def collect_power_configs() -> List[Tuple[Path, Path]]:
    """收集所有需要执行的功耗收集任务"""
    tasks = []

    for framework, hardware in FRAMEWORK_TASKS:
        for app in EXPERIMENT_CONFIG["applications"]:
            for dataset in EXPERIMENT_CONFIG["datasets"]:
                for batch_size in EXPERIMENT_CONFIG["batchSizes"]:
                    for delete_ratio in EXPERIMENT_CONFIG["deleteRatios"]:
                        for cores in EXPERIMENT_CONFIG["number_of_cores"]:
                            # stats 目录
                            stats_dir = (
                                STATS_FOLDER /
                                framework /
                                hardware /
                                app /
                                dataset /
                                str(batch_size) /
                                str(delete_ratio) /
                                EXPERIMENT_CONFIG["type"] /
                                str(cores)
                            )

                            # 输出目录
                            output_dir = (
                                OUTPUT_FOLDER /
                                framework /
                                hardware /
                                app /
                                dataset /
                                str(batch_size) /
                                str(delete_ratio) /
                                EXPERIMENT_CONFIG["type"] /
                                str(cores)
                            )

                            if stats_dir.exists():
                                tasks.append((stats_dir, output_dir))
                            else:
                                print(f"Warning: Stats dir not found: {stats_dir}")

    return tasks


def main():
    """主执行函数"""
    print("Starting power collection...")
    print(f"Working directory: {ROOT}")
    print(f"mcpat script: {MCPAT_SCRIPT}")

    tasks = collect_power_configs()
    print(f"Found {len(tasks)} tasks to run")

    if not tasks:
        print("No tasks found. Exiting.")
        return

    results = []

    if MAX_WORKERS > 1:
        print(f"Running in parallel with {MAX_WORKERS} workers")
        with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
            future_to_task = {
                executor.submit(execute_power_collection, stats, out): (stats, out)
                for stats, out in tasks
            }
            for future in as_completed(future_to_task):
                stats, out = future_to_task[future]
                try:
                    result = future.result()
                    results.append(result)
                    _, rc, t = result
                    status = "SUCCESS" if rc == 0 else "FAILED"
                    print(f"{status}: {stats} - {t:.2f}s")
                except Exception as e:
                    print(f"Exception processing {stats}: {e}")
                    results.append((stats, -1, 0.0))
    else:
        print("Running in serial mode")
        for stats, out in tasks:
            result = execute_power_collection(stats, out)
            results.append(result)
            _, rc, t = result
            status = "SUCCESS" if rc == 0 else "FAILED"
            print(f"{status}: {stats} - {t:.2f}s")

    # 摘要
    successful = sum(1 for _, rc, _ in results if rc == 0)
    failed = len(results) - successful
    total_time = sum(t for _, _, t in results)
    avg_time = total_time / len(results) if results else 0

    print("\n" + "=" * 50)
    print("POWER COLLECTION SUMMARY")
    print("=" * 50)
    print(f"Total tasks: {len(results)}")
    print(f"Successful: {successful} ({successful/len(results)*100:.1f}%)")
    print(f"Failed: {failed} ({failed/len(results)*100:.1f}%)")
    print(f"Total time: {total_time:.2f} seconds")
    print(f"Average per task: {avg_time:.2f} seconds")
    print("=" * 50)


if __name__ == "__main__":
    main()
