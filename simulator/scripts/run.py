import os
import subprocess

# 定义要遍历的根文件夹路径
ROOT = os.getcwd() + "/"
root_folder = ROOT + "config_files/ooo/stream_prefetch"
cmd = ROOT + "build/debug/zsim"

# 递归遍历文件夹及子目录下的所有.cfg文件
for foldername, subfolders, filenames in os.walk(root_folder):
    for filename in filenames:
        if filename.endswith('.cfg'):
            cfg_file_path = os.path.join(foldername, filename)
            # 执行./cmd .cfg命令
            return_code = subprocess.call([cmd, cfg_file_path])
            if(return_code != 0):
                print("Error: " + cfg_file_path)
                exit(1)
