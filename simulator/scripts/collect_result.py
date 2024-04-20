from __future__ import print_function
import sys
import os
from get_stats_per_app import get_results
import matplotlib
matplotlib.use('tkagg') # Must be before importing matplotlib.pyplot or pylab!
import matplotlib.pyplot as plt
import numpy as np
from collections import defaultdict

core_type = ["inorder", "inorder_nuca", "ooo", "ooo_nuca"]
prefetch_type = ["no_prefetch","stream_prefetch", "graph_prefetch"]
core_num = [1, 2, 4, 8]
graph_prefetch_num = [1, 2, 4, 8, 16]
stream_prefetch_num = [16]

RESULTS_PATH = os.getcwd() + "/zsim_stats_graph"

# 只找当前目录（不含子目录）.out结尾的文件并返回完整路径
def get_file_path(curDir):
    for file in os.listdir(curDir):
        if file.endswith(".out"):
            return os.path.join(curDir, file)
    return ""

# 对于不同类型的prefetch_type，结果存放的目录如下：
# no_prefetch: RESULTS_PATH/core_type/no_prefetch/core_num/result.out
# stream_prefetch: RESULTS_PATH/core_type/stream_prefetch/stream_prefetch_num/core_num/result.out
# graph_prefetch: RESULTS_PATH/core_type/graph_prefetch/graph_prefetch_num/core_num/result.out
def collect_results():
    results = defaultdict(dict)
    
    for ct in core_type:
        for pt in prefetch_type:
            if pt == "no_prefetch":
                for cn in core_num:
                    result_path = RESULTS_PATH + "/" + ct + "/" + pt + "/" + str(cn)
                    result_file = get_file_path(result_path)
                    if os.path.exists(result_file):
                        results[(ct, pt, cn)] = get_results(result_file)
                    else:
                        print("Results for " + ct + " " + pt + " " + str(cn) + " do not exist")
            elif pt == "stream_prefetch":
                for cn in core_num:
                    for sn in stream_prefetch_num:
                        result_path = RESULTS_PATH + "/" + ct + "/" + pt + "/" + str(cn) + "/" + str(sn)
                        result_file = get_file_path(result_path)
                        if os.path.exists(result_file):
                            results[(ct, pt, cn, sn)] = get_results(result_file)
                        else:
                            print("Results for " + ct + " " + pt + " " + str(cn) + " " + str(sn) + " do not exist")
            elif pt == "graph_prefetch":
                for cn in core_num:
                    for gn in graph_prefetch_num:
                        result_path = RESULTS_PATH + "/" + ct + "/" + pt + "/" + str(cn) + "/" + str(gn)
                        result_file = get_file_path(result_path)
                        if os.path.exists(result_file):
                            results[(ct, pt, cn, gn)] = get_results(result_file)
                        else:
                            print("Results for " + ct + " " + pt + " " + str(cn) + " " + str(gn) + " do not exist")
    
    return results


if __name__ == "__main__":
    # 收集信息
    results = collect_results()
    # 绘制柱状图matpilot（针对ooo, 对比no_prefetch和stream_prefetch为16时stream_prefetch在不同core num下的cycles对比）
    # 1. ooo no_prefetch
    # 2. ooo stream_prefetch 16
    # 3. ooo graph_prefetch 16
    # 横坐标为core num，纵坐标为cycles
    ct = "ooo_nuca"
    no_prefetch_cycles = []
    stream_prefetch_cycles = []
    graph_prefetch_cycles = []
    x_values = np.arange(len(core_num))

    for cn in core_num:
        no_prefetch_cycles.append(results[(ct, "no_prefetch", cn)]["cycles"])
        stream_prefetch_cycles.append(results[(ct, "stream_prefetch", cn, 16)]["cycles"])
        graph_prefetch_cycles.append(results[(ct, "graph_prefetch", cn, 16)]["cycles"])

    bar_width = 0.2  # 设置柱子的宽度

    # 绘制柱状图
    plt.bar(x_values - bar_width, no_prefetch_cycles, width=bar_width, color='b', label='no_prefetch')
    plt.bar(x_values, stream_prefetch_cycles, width=bar_width, color='r', label='stream_prefetch')
    plt.bar(x_values + bar_width, graph_prefetch_cycles, width=bar_width, color='g', label='graph_prefetch')

    # 设置横坐标刻度
    plt.xticks(x_values, core_num)

    # 添加标题和标签
    plt.xlabel('Core Num')
    plt.ylabel('Cycles')
    plt.title('Cycles vs Core Num')
    plt.legend()

    # 显示图表
    plt.show()

    # ct = "ooo"
    # # 设置每组的宽度
    # bar_width = 0.35

    # # 设置每个组的位置
    # index = np.arange(len(core_num))

    # # 循环生成每个 prefetch_num 对应的柱状图
    # for i, prefetch in enumerate(graph_prefetch_num):
    #     cycle_num = []
    #     for cn in core_num:
    #         cycle_num.append(results[(ct, "graph_prefetch", cn, prefetch)]["cycles"])
    #     plt.bar(index + i * bar_width / len(graph_prefetch_num), cycle_num, bar_width / len(graph_prefetch_num), label=f'prefetch_num {graph_prefetch_num[i]}')

    # # 添加标签
    # plt.xlabel('Core nums')
    # plt.ylabel('Cycles')
    # plt.title('Cycle vs Core Num')
    # plt.xticks(index + bar_width / 2, core_num)
    # plt.legend()

    # plt.show()

    sys.exit(0)