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
        },
        "soc-LiveJournal1" : {
            "source" : 0, "direction": "",
        },
        "com-lj.ungraph" : {
            "source" : 0, "direction": "-s",
        },
        "wiki-Talk" : {
            "source" : 2, "direction" : "",
        },
        "com-orkut.ungraph" : {
            "source" : 1, "direction" : "-s",
        },
    },
    "hardware": {
        "cores_configs": [(1, 2)],
        "type": "ooo_nuca",
        "bank_size": 0.5 * 1024 * 1024,
    },
    "prefetch": {
        "graphPrefetchEntries": [(4, 32), (8, 32), (16, 32), (32, 32), (64, 32)],
        "PDELatency": 2,
    },
    "paths": {
        "DATASET_ROOT":  Path("/home/xuanyi/research/graph/RisGraph") / "data",
        "ROOT": Path("/home/xuanyi/research/simulator/DAMOV/simulator"),
        "TEMPLATE_DIR": Path("/home/xuanyi/research/simulator/DAMOV/simulator/templates"),
        "FRAMEWORK_DIR" : Path("/home/xuanyi/research/graph/graphbolt/app/synthetic"),
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

            for cores, mc in cfg["hardware"]["cores_configs"]:
                # 路径构建
                config_dir = build_paths(cfg, framework, hardware, app_name, name, cores)

                # 命令构建
                command = build_command(app_name, workload_root, cores, dataSet, cfg["paths"]["DATASET_ROOT"], name)

                # 替换参数
                replacements = {
                    "NUMBER_CORES": str(cores),
                    "LLC_SIZE": str(int(cores * cfg["hardware"]["bank_size"])),
                    "STATS_PATH": "result",
                    "COMMAND_STRING": f'"{command}";',
                    "NUMBER_CONTROLLERS": str(mc),
                    "WEIGHT_ENABLE": str(weightEnable),
                    "ALGORITHM": f"\"{app_name}\"",
                    **extra_replacements
                }

                # 生成配置文件
                generate_config_file(config_dir, template_content, replacements)

def build_paths(cfg, framework, hardware, app, dataset, cores):
    """构建配置和统计目录路径"""
    config_dir = (
        cfg["paths"]["ROOT"] / "config_files" / framework / hardware / app /
        dataset / cfg["hardware"]["type"] / str(cores)
    )

    stats_dir = (
        cfg["paths"]["ROOT"] / "zsim_stats" / framework / hardware / app /
        dataset / cfg["hardware"]["type"] / str(cores)
    )

    mkdir_p(config_dir)

    return config_dir

def build_command(app, workload_root, cores, dataset_config,
                  dataset_root, dataset_name):
    """构建执行命令"""
    dataset_path = dataset_root / f"{dataset_name}.bin"
    return f"{workload_root}/{app} {dataset_config['direction']} -source {dataset_config['source']} -nWorkers {cores} -batch 0 -import 1 -del 0 {dataset_path}"

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
    PDELatency = cfg["prefetch"]["PDELatency"]

    base_tasks = [
        {
            "framework": "kickstarter",
            "hardware": "base",
            "template": "template_{type}.cfg",
            "workload_path": "base",
            "extra": {}
        },
        {
            "framework": "kickstarter",
            "hardware": "L1_64",
            "template": "template_{type}_L1_64.cfg",
            "workload_path": "base",
            "extra": {}
        },
        {
            "framework": "kickstarter",
            "hardware": "stride_prefetch",
            "template": "template_{type}_stride_prefetch.cfg",
            "workload_path": "base",
            "extra": {}
        },
        # {
        #     "framework": "kickstarter",
        #     "hardware": "ampm_prefetch",
        #     "template": "template_{type}_ampm_prefetch.cfg",
        #     "workload_path": "base",
        #     "extra": {}
        # },
        # {
        #     "framework": "kickstarter",
        #     "hardware": "bop_prefetch",
        #     "template": "template_{type}_bop_prefetch.cfg",
        #     "workload_path": "base",
        #     "extra": {}
        # },
        # {
        #     "framework": "kickstarter",
        #     "hardware": "imp_prefetch",
        #     "template": "template_{type}_imp_prefetch.cfg",
        #     "workload_path": "base",
        #     "extra": {}
        # },
    ]
    graph_prefetch_tasks = [
        {
            "framework": "kickstarter",
            "hardware": f"graph_prefetch_{root}_{dest}_{PDELatency}",
            "template": "template_{type}_graph_prefetch.cfg",
            "workload_path": "base_prefetch",
            "extra": {"FILTERENABLE": "False", "ROOT_ENTRIES": root, "DEST_ENTRIES": dest, "PDE_LATENCY": PDELatency},
        }
        for root, dest in cfg["prefetch"]["graphPrefetchEntries"]
    ]
    graph_filter_tasks = [
        {
            "framework": "kickstarter",
            "hardware": f"graph_prefetch_with_filter_{root}_{dest}_{PDELatency}",
            "template": "template_{type}_graph_prefetch.cfg",
            "workload_path": "base_prefetch",
            "extra": {"FILTERENABLE": "True",  "ROOT_ENTRIES": root, "DEST_ENTRIES": dest, "PDE_LATENCY": PDELatency},
        }
        for root, dest in cfg["prefetch"]["graphPrefetchEntries"]
    ]
    return base_tasks + graph_prefetch_tasks + graph_filter_tasks

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


if __name__ == "__main__":
    cfg = config
    validate_config(cfg)
    for task in get_tasks():
        generate_configs(
            cfg,
            framework=task["framework"],
            hardware=task["hardware"],
            template=task["template"].format(type=cfg["hardware"]["type"]),
            workload_root=cfg["paths"]["FRAMEWORK_DIR"]/ task["workload_path"],
            extra_replacements=task["extra"],
        )

    print_generation_summary(cfg)