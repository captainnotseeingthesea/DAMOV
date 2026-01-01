from __future__ import print_function
from pathlib import Path
import os 
import errno

# 将配置拆分为更清晰的结构
config = {
    "applications": [
        {"name": "SSSP", "weighted": True},
        {"name": "SSWP", "weighted": True},
        {"name": "BFS", "weighted": False},
        {"name": "CC", "weighted": False},
    ],
    "datasets": {
        "roadNet-CA-ungraph": {
            "source": 0, "direction": "-s",
            "recordBatch": {"SSSP": (7, 300), "SSWP": (7, 1), "BFS": (7, 1), "CC": (7, 300)}
        },
        "sc-ldoor": {
            "source": 1, "direction": "-s",
            "recordBatch": {"SSSP": (2, 300), "SSWP": (1, 1), "BFS": (10, 1), "CC": (1, 300)}
        },
        "delaunay-n21": {
            "source": 1, "direction": "-s",
            "recordBatch": {"SSSP": (1, 300), "SSWP": (1, 1), "BFS": (1, 1), "CC": (1, 300)}
        },
        "venturiLevel3": {
            "source": 1, "direction": "-s",
            "recordBatch": {"SSSP": (1, 300), "SSWP": (1, 1), "BFS": (1, 1), "CC": (1, 30)}
        },
        "hugetrace-00000": {
            "source": 1, "direction": "-s",
            "recordBatch": {"SSSP": (15, 300), "SSWP": (15, 1), "BFS": (15, 1), "CC": (15, 10000)}
        },
        "apache2": {
            "source": 1, "direction": "-s",
            "recordBatch": {"SSSP": (1, 300), "SSWP": (1, 1), "BFS": (1, 1), "CC": (1, 300)}
        },
        "ecology1": {
            "source": 1, "direction": "-s",
            "recordBatch": {"SSSP": (7, 300), "SSWP": (7, 1), "BFS": (8, 1), "CC": (1, 300)}
        }
    },
    "hardware": {
        "batchSizes": [100000],
        "deleteRatios": [0.1],
        "cores_configs": [(16, 6)],
        "type": "ooo_nuca",
        "bank_size": 0.5 * 1024 * 1024,
        "buckets": 128,
    },
    "prefetch": {
        "graphPrefetchEntries": [(4, 32), (8, 64)],
        "PDELatencies": [2, 8, 16, 27],
    },
    "paths": {
        "DATASET_ROOT": Path("/home/xuanyi/research/graph/graphbolt/inputs"),
        "ROOT": Path("/home/xuanyi/research/simulator/DAMOV/simulator"),
        "TEMPLATE_DIR": Path("/home/xuanyi/research/simulator/DAMOV/simulator/templates"),
    }
}

def mkdir_p(directory):
    try:
        os.makedirs(directory)
    except OSError as exc:
        # Python >2.5
        if exc.errno == errno.EEXIST and os.path.isdir(directory):
            pass
        else:
            raise

def process_template_line(line, replacements):
    """统一处理模板替换"""
    for key, value in replacements.items():
        line = line.replace(key, str(value))
    return line

def generate_configs(cfg, framework: str, hardware: str, template: str, 
                    workload_root: Path, extra_replacements=None):
    extra_replacements = extra_replacements or {}
    
    template_path = cfg["paths"]["TEMPLATE_DIR"] / template
    if not template_path.exists():
        print(f"Warning: Template {template_path} not found")
        return
    
    template_content = template_path.read_text()
    
    for app in cfg["applications"]:
        app_name = app["name"]
        weightEnable = app["weighted"]
        
        for name, dataSet in cfg["datasets"].items():
            if app_name not in dataSet["recordBatch"]:
                continue
                
            recordBatch, delta = dataSet["recordBatch"][app_name]
            
            for batchSize in cfg["hardware"]["batchSizes"]:
                for deleteRatio in cfg["hardware"]["deleteRatios"]:
                    for cores, mc in cfg["hardware"]["cores_configs"]:
                        # 路径构建
                        config_dir, stats_dir = build_paths(cfg, framework, hardware, 
                                                          app_name, name, batchSize, 
                                                          deleteRatio, cores)
                        
                        # 命令构建
                        command = build_command(app_name, workload_root, cores, dataSet, 
                                              recordBatch, batchSize, deleteRatio, 
                                              cfg["paths"]["DATASET_ROOT"], name)
                        
                        # 替换参数
                        replacements = {
                            "NUMBER_CORES": str(cores),
                            "LLC_SIZE": str(int(cores * cfg["hardware"]["bank_size"])),
                            "STATS_PATH": str(stats_dir / "result"),
                            "COMMAND_STRING": f'"{command}";',
                            "NUMBER_CONTROLLERS": str(mc),
                            "DELTA": str(delta),
                            "BUCKETS": str(cfg["hardware"]["buckets"]),
                            "WEIGHT_ENABLE": str(weightEnable),
                            "ALGORITHM": app_name,
                            **extra_replacements
                        }
                        
                        # 生成配置文件
                        generate_config_file(config_dir, template_content, replacements)

def build_paths(cfg, framework, hardware, app, dataset, batchSize, deleteRatio, cores):
    """构建配置和统计目录路径"""
    config_dir = (
        cfg["paths"]["ROOT"] / "config_files" / framework / hardware / app / 
        dataset / str(batchSize) / str(deleteRatio) / cfg["hardware"]["type"] / str(cores)
    )
    
    stats_dir = (
        cfg["paths"]["ROOT"] / "zsim_stats" / framework / hardware / app / 
        dataset / str(batchSize) / str(deleteRatio) / cfg["hardware"]["type"] / str(cores)
    )
    
    mkdir_p(config_dir)
    mkdir_p(stats_dir)
    
    return config_dir, stats_dir

def build_command(app, workload_root, cores, dataset_config, 
                 recordBatch, batchSize, deleteRatio, dataset_root, dataset_name):
    """构建执行命令"""
    return (
        f"{workload_root}/{app} -recordBatch {recordBatch} -nWorkers {cores} "
        f"-source {dataset_config['source']} {dataset_config['direction']} "
        f"-numberOfUpdateBatches {recordBatch} -nEdges {batchSize} "
        f"-streamPath {dataset_root}/op/{dataset_name}_{batchSize}_{deleteRatio}.op "
        f"{dataset_root}/adj/{dataset_name}.adj"
    )

def generate_config_file(config_dir, template_content, replacements):
    """生成配置文件"""
    config_path = config_dir / "zsim.cfg"
    
    processed_content = []
    for line in template_content.splitlines(keepends=True):
        processed_line = process_template_line(line, replacements)
        processed_content.append(processed_line)
    
    config_path.write_text(''.join(processed_content))
    print(f"Generated config: {config_path}")

def get_tasks():
    """返回任务配置"""
    PDE_4_32_2 = {"ROOT_ENTRIES": 4, "DEST_ENTRIES": 32, "PDE_LATENCY": 2}
    PDE_4_32_8 = {"ROOT_ENTRIES": 4, "DEST_ENTRIES": 32, "PDE_LATENCY": 8}
    PDE_4_32_16 = {"ROOT_ENTRIES": 4, "DEST_ENTRIES": 32, "PDE_LATENCY": 16}
    PDE_4_32_27 = {"ROOT_ENTRIES": 4, "DEST_ENTRIES": 32, "PDE_LATENCY": 27}
    
    return [
        # Kickstarter 配置
        {
            "framework": "kickstarter",
            "hardware": "base",
            "template": "template_{type}.cfg",
            "workload_path": "graphbolt/base",
            "extra": {}
        },
        {
            "framework": "kickstarter",
            "hardware": "L1_64",
            "template": "template_{type}_L1_64.cfg",
            "workload_path": "graphbolt/base",
            "extra": {}
        },
        {
            "framework": "kickstarter", 
            "hardware": "graph_prefetch_4_32_2",
            "template": "template_{type}_graph_prefetch.cfg",
            "workload_path": "graphbolt/base_prefetch",
            "extra": {"FILTERENABLE": "False", **PDE_4_32_2}
        },
        {
            "framework": "kickstarter",
            "hardware": "stride_prefetch",
            "template": "template_{type}_stride_prefetch.cfg",
            "workload_path": "graphbolt/base",
            "extra": {}
        },
        {
            "framework": "kickstarter",
            "hardware": "ampm_prefetch",
            "template": "template_{type}_ampm_prefetch.cfg",
            "workload_path": "graphbolt/base",
            "extra": {}
        },
        {
            "framework": "kickstarter",
            "hardware": "bop_prefetch",
            "template": "template_{type}_bop_prefetch.cfg",
            "workload_path": "graphbolt/base",
            "extra": {}
        },
        {
            "framework": "kickstarter",
            "hardware": "imp_prefetch",
            "template": "template_{type}_imp_prefetch.cfg",
            "workload_path": "graphbolt/base",
            "extra": {}
        },
        {
            "framework": "kickstarter",
            "hardware": "graph_prefetch_with_filter_4_32_2",
            "template": "template_{type}_graph_prefetch.cfg",
            "workload_path": "graphbolt/base_prefetch",
            "extra": {"FILTERENABLE": "True", **PDE_4_32_2}
        },

        # WSGraph 配置
        {
            "framework": "wsgraph",
            "hardware": "base", 
            "template": "template_{type}.cfg",
            "workload_path": "graphbolt/wsgraph-s",
            "extra": {}
        },
        {
            "framework": "wsgraph",
            "hardware": "L1_64", 
            "template": "template_{type}_L1_64.cfg",
            "workload_path": "graphbolt/wsgraph-s",
            "extra": {}
        },
        {
            "framework": "wsgraph",
            "hardware": "stride_prefetch",
            "template": "template_{type}_stride_prefetch.cfg",
            "workload_path": "graphbolt/wsgraph_s",
            "extra": {}
        },
        {
            "framework": "wsgraph",
            "hardware": "ampm_prefetch",
            "template": "template_{type}_ampm_prefetch.cfg",
            "workload_path": "graphbolt/wsgraph_s",
            "extra": {}
        },
        {
            "framework": "wsgraph",
            "hardware": "bop_prefetch",
            "template": "template_{type}_bop_prefetch.cfg",
            "workload_path": "graphbolt/wsgraph_s",
            "extra": {}
        },
        {
            "framework": "wsgraph",
            "hardware": "imp_prefetch",
            "template": "template_{type}_imp_prefetch.cfg",
            "workload_path": "graphbolt/wsgraph_s",
            "extra": {}
        },
        {
            "framework": "wsgraph",
            "hardware": "graph_prefetch_4_32_2",
            "template": "template_{type}_graph_prefetch.cfg",
            "workload_path": "graphbolt/wsgraph",
            "extra": {"FILTERENABLE": "False", **PDE_4_32_2}
        },
        {
            "framework": "wsgraph",
            "hardware": "graph_prefetch_with_filter_4_32_2",
            "template": "template_{type}_graph_prefetch.cfg",
            "workload_path": "graphbolt/wsgraph",
            "extra": {"FILTERENABLE": "True", **PDE_4_32_2}
        },
    ]
    # return [
    #     {
    #         "framework": "kickstarter", 
    #         "hardware": "graph_prefetch_4_32_8",
    #         "template": "template_{type}_graph_prefetch.cfg",
    #         "workload_path": "graphbolt/base_prefetch",
    #         "extra": {"FILTERENABLE": "False", **PDE_4_32_8}
    #     },
    #     {
    #         "framework": "kickstarter", 
    #         "hardware": "graph_prefetch_4_32_16",
    #         "template": "template_{type}_graph_prefetch.cfg",
    #         "workload_path": "graphbolt/base_prefetch",
    #         "extra": {"FILTERENABLE": "False", **PDE_4_32_16}
    #     },
    #     {
    #         "framework": "kickstarter", 
    #         "hardware": "graph_prefetch_4_32_27",
    #         "template": "template_{type}_graph_prefetch.cfg",
    #         "workload_path": "graphbolt/base_prefetch",
    #         "extra": {"FILTERENABLE": "False", **PDE_4_32_27}
    #     },
    #     {
    #         "framework": "wsgraph",
    #         "hardware": "graph_prefetch_with_filter_4_32_8",
    #         "template": "template_{type}_graph_prefetch.cfg",
    #         "workload_path": "graphbolt/wsgraph",
    #         "extra": {"FILTERENABLE": "True", **PDE_4_32_8}
    #     },
    #     {
    #         "framework": "wsgraph",
    #         "hardware": "graph_prefetch_with_filter_4_32_16",
    #         "template": "template_{type}_graph_prefetch.cfg",
    #         "workload_path": "graphbolt/wsgraph",
    #         "extra": {"FILTERENABLE": "True", **PDE_4_32_16}
    #     },
    #     {
    #         "framework": "wsgraph",
    #         "hardware": "graph_prefetch_with_filter_4_32_2",
    #         "template": "template_{type}_graph_prefetch.cfg",
    #         "workload_path": "graphbolt/wsgraph",
    #         "extra": {"FILTERENABLE": "True", **PDE_4_32_27}
    #     },
    # ]

def validate_config(cfg):
    """验证配置完整性"""
    required_keys = ["applications", "datasets", "hardware", "paths"]
    for key in required_keys:
        if key not in cfg:
            raise ValueError(f"Missing required config key: {key}")
        
def count_generated_files(config_dir):
    """统计生成的配置文件数量"""
    return len(list(config_dir.rglob("zsim.cfg")))

def print_generation_summary(cfg):
    """打印生成摘要"""
    total_files = count_generated_files(cfg["paths"]["ROOT"] / "config_files")
    print(f"\nGeneration Summary:")
    print(f"Total config files: {total_files}")
    print(f"Applications: {len(cfg['applications'])}")
    print(f"Datasets: {len(cfg['datasets'])}")
    print(f"Hardware configurations: {len(cfg['hardware']['cores_configs'])}")

def load_config():
    """加载或返回默认配置"""
    # 这里可以扩展为从文件加载配置
    return config

if __name__ == "__main__":
    cfg = load_config()  # 可以从外部文件加载配置
    validate_config(cfg)
    for task in get_tasks():
        generate_configs(
            cfg,
            framework=task["framework"],
            hardware=task["hardware"],
            template=task["template"].format(type=cfg["hardware"]["type"]),
            workload_root=cfg["paths"]["ROOT"].parent / task["workload_path"],
            extra_replacements=task["extra"],
        )
    
    print_generation_summary(cfg)