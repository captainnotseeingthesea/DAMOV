import os
import subprocess
from concurrent.futures import ProcessPoolExecutor, as_completed
import time
import psutil
from pathlib import Path
from typing import List, Tuple

# 定义根目录路径
ROOT = Path(os.getcwd())
CONFIG_FOLDER = ROOT / "config_files"
OUTPUT_FOLDER = ROOT / "zsim_stats"

# 模拟器配置
SIMULATOR_PATH = ROOT / "build" / "debug" / "zsim"
MAX_WORKERS = 1
CPU_USAGE_THRESHOLD = 80

# 实验配置参数（与生成脚本保持一致）
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

# 框架和硬件配置组合
FRAMEWORK_TASKS = [
    ("kickstarter", "base"),
    ("kickstarter", "L1_64"),
    ("kickstarter", "stride_prefetch"),
    ("kickstarter", "ampm_prefetch"), 
    ("kickstarter", "bop_prefetch"),
    ("kickstarter", "imp_prefetch"),
    ("kickstarter", "graph_prefetch_4_32_2"),
    ("kickstarter", "graph_prefetch_with_filter_4_32_2"),
    ("wsgraph", "base"),
    ("wsgraph", "L1_64"),
    ("wsgraph", "stride_prefetch"),
    ("wsgraph", "ampm_prefetch"),
    ("wsgraph", "bop_prefetch"),
    ("wsgraph", "imp_prefetch"),
    ("wsgraph", "graph_prefetch_4_32_2"),
    ("wsgraph", "graph_prefetch_with_filter_4_32_2"),
]


def execute_simulation(cfg_file_path: Path, simulator_cmd: Path) -> Tuple[Path, int, float]:
    """执行单个模拟命令
    
    Args:
        cfg_file_path: 配置文件路径
        simulator_cmd: 模拟器可执行文件路径
        
    Returns:
        元组(配置文件路径, 返回码, 执行时间)
    """
    start_time = time.time()
    
    # 构建对应的输出文件路径
    relative_path = cfg_file_path.relative_to(CONFIG_FOLDER)
    output_file_path = OUTPUT_FOLDER / relative_path.with_suffix('.log')
    
    # 确保输出目录存在
    output_file_path.parent.mkdir(parents=True, exist_ok=True)
    
    print(f"Executing: {simulator_cmd.name} {cfg_file_path} -> {output_file_path}")
    
    try:
        # 执行模拟命令并重定向输出
        with open(output_file_path, 'w') as output_file:
            return_code = subprocess.call(
                [str(simulator_cmd), str(cfg_file_path)], 
                stdout=output_file, 
                stderr=subprocess.STDOUT
            )
        
        execution_time = time.time() - start_time
        
        # 记录执行时间到日志文件
        with open(output_file_path, 'a') as output_file:
            output_file.write(f"\n\nExecution time: {execution_time:.2f} seconds\n")
        
        if return_code != 0:
            print(f"Error: {cfg_file_path} failed with return code {return_code}")
            
        return cfg_file_path, return_code, execution_time
        
    except Exception as e:
        print(f"Exception occurred while executing {cfg_file_path}: {e}")
        return cfg_file_path, -1, 0.0


def collect_simulation_configs() -> List[Path]:
    """收集所有需要执行的配置文件路径
    
    Returns:
        配置文件路径列表
    """
    config_files = []
    
    for framework, hardware in FRAMEWORK_TASKS:
        for app_name in EXPERIMENT_CONFIG["applications"]:
            for dataset_name in EXPERIMENT_CONFIG["datasets"]:
                for batch_size in EXPERIMENT_CONFIG["batchSizes"]:
                    for delete_ratio in EXPERIMENT_CONFIG["deleteRatios"]:
                        for cores in EXPERIMENT_CONFIG["number_of_cores"]:
                            # 构建配置文件路径
                            config_path = (
                                CONFIG_FOLDER / 
                                framework / 
                                hardware / 
                                app_name / 
                                dataset_name / 
                                str(batch_size) / 
                                str(delete_ratio) / 
                                EXPERIMENT_CONFIG["type"] / 
                                str(cores) / 
                                "zsim.cfg"
                            )
                            
                            if config_path.exists():
                                config_files.append(config_path)
                            else:
                                print(f"Warning: Configuration file not found: {config_path}")
    
    return config_files


def main():
    """主执行函数"""
    print("Starting simulation execution...")
    print(f"Working directory: {ROOT}")
    print(f"Simulator path: {SIMULATOR_PATH}")
    
    # 收集所有配置文件
    config_files = collect_simulation_configs()
    print(f"Found {len(config_files)} configuration files to execute")
    
    if not config_files:
        print("No configuration files found. Exiting.")
        return
    
    # 验证模拟器是否存在
    if not SIMULATOR_PATH.exists():
        print(f"Error: Simulator not found at {SIMULATOR_PATH}")
        return
    if not os.access(SIMULATOR_PATH, os.X_OK):
        print(f"Error: Simulator is not executable: {SIMULATOR_PATH}")
        return
    
    # 执行模拟任务
    results = []
    
    if MAX_WORKERS > 1:
        # 并行执行模式
        print(f"Running in parallel mode with {MAX_WORKERS} workers")
        with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
            # 提交所有任务
            future_to_config = {
                executor.submit(execute_simulation, cfg_file, SIMULATOR_PATH): cfg_file 
                for cfg_file in config_files
            }
            
            # 处理完成的任务
            for future in as_completed(future_to_config):
                config_file = future_to_config[future]
                try:
                    result = future.result()
                    results.append(result)
                    cfg_path, return_code, exec_time = result
                    status = "SUCCESS" if return_code == 0 else "FAILED"
                    print(f"{status}: {config_file} - Time: {exec_time:.2f}s")
                except Exception as e:
                    print(f"Exception processing {config_file}: {e}")
                    results.append((config_file, -1, 0.0))
    else:
        # 串行执行模式
        print("Running in serial mode")
        for config_file in config_files:
            result = execute_simulation(config_file, SIMULATOR_PATH)
            results.append(result)
            cfg_path, return_code, exec_time = result
            status = "SUCCESS" if return_code == 0 else "FAILED"
            print(f"{status}: {config_file} - Time: {exec_time:.2f}s")
    
    # 生成执行摘要
    successful = sum(1 for _, return_code, _ in results if return_code == 0)
    failed = len(results) - successful
    total_time = sum(exec_time for _, _, exec_time in results)
    avg_time = total_time / len(results) if results else 0
    
    print("\n" + "="*50)
    print("EXECUTION SUMMARY")
    print("="*50)
    print(f"Total tasks executed: {len(results)}")
    print(f"Successful: {successful} ({successful/len(results)*100:.1f}%)")
    print(f"Failed: {failed} ({failed/len(results)*100:.1f}%)")
    print(f"Total execution time: {total_time:.2f} seconds")
    print(f"Average time per task: {avg_time:.2f} seconds")
    print("="*50)


if __name__ == "__main__":
    main()