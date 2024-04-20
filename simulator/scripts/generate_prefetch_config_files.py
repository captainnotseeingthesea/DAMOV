from __future__ import print_function
import sys
import os
import errno

os.chdir("../workloads")
WORKLOAD_ROOT = os.getcwd() +"/"
os.chdir("../simulator") 
ROOT = os.getcwd() +"/"

def mkdir_p(directory):
    try:
        os.makedirs(directory)
    except OSError as exc:  # Python >2.5
        if exc.errno == errno.EEXIST and os.path.isdir(directory):
            pass
        else:
            raise

def create_configs_no_prefetch(benchmark, application, dataSet, command, version):
    number_of_cores = [1, 2, 4, 8]

    for cores in number_of_cores:
        mkdir_p(ROOT+"config_files/"+version+"/no_prefetch/"+benchmark+"/"+str(cores)+"/")

    for cores in number_of_cores:
        mkdir_p(ROOT+"zsim_stats/"+version+"/no_prefetch/"+str(cores)+"/")

    for cores in number_of_cores:
        with open(ROOT+"templates/template_"+version+".cfg", "r") as ins:
            config_file = open(ROOT+"config_files/"+version+"/no_prefetch/"+benchmark+"/"+str(cores)+"/"+application+"_"+dataSet+".cfg","w")
            for line in ins:
                line = line.replace("NUMBER_CORES", str(cores))
                line = line.replace("STATS_PATH", "zsim_stats/"+version+"/no_prefetch/"+str(cores)+"/"+benchmark+"_"+application+"_"+dataSet)
                line = line.replace("COMMAND_STRING", "\"" + command + "\";")
                line = line.replace("WORKLOAD_ROOT",WORKLOAD_ROOT)

                config_file.write(line)
            config_file.close()
        ins.close()

def create_nuca_configs_no_prefetch(benchmark, application, dataSet, command, version):
    number_of_cores = [1, 2, 4, 8]
    bank_size = 0.5 * 1024 * 1024 # 0.5MB/bank

    for cores in number_of_cores:
        mkdir_p(ROOT+"config_files/"+version+"_nuca/no_prefetch/"+benchmark+"/"+str(cores)+"/")

    for cores in number_of_cores:
        mkdir_p(ROOT+"zsim_stats/"+version+"_nuca/no_prefetch/"+str(cores)+"/")

    for cores in number_of_cores:
        with open(ROOT+"templates/template_"+version+"_nuca.cfg", "r") as ins:
            config_file = open(ROOT+"config_files/"+version+"_nuca/no_prefetch/"+benchmark+"/"+str(cores)+"/"+application+"_"+dataSet+".cfg","w")
            for line in ins:
                line = line.replace("NUMBER_CORES", str(cores))
                line = line.replace("LLC_SIZE", str(int(cores * bank_size)))
                line = line.replace("STATS_PATH", "zsim_stats/"+version+"_nuca/no_prefetch/"+str(cores)+"/"+benchmark+"_"+application+"_"+dataSet)
                line = line.replace("COMMAND_STRING", "\"" + command + "\";")
                line = line.replace("WORKLOAD_ROOT",WORKLOAD_ROOT)

                config_file.write(line)
            config_file.close()
        ins.close()

def create_configs_stream_prefetch(benchmark, application, dataSet, command, version):
    number_of_cores = [1, 2, 4, 8]
    number_of_entries = [16]

    for cores in number_of_cores:
        for entries in number_of_entries:
            mkdir_p(ROOT+"config_files/"+version+"/stream_prefetch/"+benchmark+"/"+str(cores)+"/"+ str(entries) + "/")

    for cores in number_of_cores:
        for entries in number_of_entries:
            mkdir_p(ROOT+"zsim_stats/"+version+"/stream_prefetch/"+str(cores)+"/"+ str(entries) + "/")

    for cores in number_of_cores:
        for entries in number_of_entries:
            with open(ROOT+"templates/template_"+version+"_stream_prefetch.cfg", "r") as ins:
                config_file = open(ROOT+"config_files/"+version+"/stream_prefetch/"+benchmark+"/"+str(cores)+"/"+ str(entries) + "/" + application+"_"+dataSet+".cfg","w")
                for line in ins:
                    line = line.replace("NUMBER_CORES", str(cores))
                    line = line.replace("STATS_PATH", "zsim_stats/"+version+"/stream_prefetch/"+str(cores)+"/"+ str(entries) + "/" +benchmark+"_"+application+"_"+dataSet)
                    line = line.replace("COMMAND_STRING", "\"" + command + "\";")
                    line = line.replace("WORKLOAD_ROOT",WORKLOAD_ROOT)
                    line = line.replace("PREFETCHER_ENTRIES", str(entries))

                    config_file.write(line)
                config_file.close()
            ins.close()

def create_nuca_configs_stream_prefetch(benchmark, application, dataSet, command, version):
    number_of_cores = [1, 2, 4, 8]
    number_of_entries = [16]
    bank_size = 0.5 * 1024 * 1024 # 0.5MB/bank

    for cores in number_of_cores:
        for entries in number_of_entries:
            mkdir_p(ROOT+"config_files/"+version+"_nuca/stream_prefetch/"+benchmark+"/"+str(cores)+"/"+ str(entries) + "/")

    for cores in number_of_cores:
        for entries in number_of_entries:
            mkdir_p(ROOT+"zsim_stats/"+version+"_nuca/stream_prefetch/"+str(cores)+"/"+ str(entries) + "/")

    for cores in number_of_cores:
        for entries in number_of_entries:
            with open(ROOT+"templates/template_"+version+"_nuca_stream_prefetch.cfg", "r") as ins:
                config_file = open(ROOT+"config_files/"+version+"_nuca/stream_prefetch/"+benchmark+"/"+str(cores)+"/"+ str(entries) + "/" + application+"_"+dataSet+".cfg","w")
                for line in ins:
                    line = line.replace("NUMBER_CORES", str(cores))
                    line = line.replace("LLC_SIZE", str(int(cores * bank_size)))
                    line = line.replace("STATS_PATH", "zsim_stats/"+version+"_nuca/stream_prefetch/"+str(cores)+"/"+ str(entries) + "/" +benchmark+"_"+application+"_"+dataSet)
                    line = line.replace("COMMAND_STRING", "\"" + command + "\";")
                    line = line.replace("WORKLOAD_ROOT",WORKLOAD_ROOT)
                    line = line.replace("PREFETCHER_ENTRIES", str(entries))

                    config_file.write(line)
                config_file.close()
            ins.close()

def create_graph_prefetch_configs(benchmark, application, dataSet, command, version):
    number_of_cores = [1, 2, 4, 8]
    number_of_entries = [1, 2, 4, 8, 16]

    for cores in number_of_cores:
        for entries in number_of_entries:
            mkdir_p(ROOT+"config_files/"+version+"/graph_prefetch/"+benchmark+"/"+str(cores)+"/"+ str(entries) + "/")

    for cores in number_of_cores:
        for entries in number_of_entries:
            mkdir_p(ROOT+"zsim_stats/"+version+"/graph_prefetch/"+str(cores)+"/"+ str(entries) + "/")

    for cores in number_of_cores:
        for entries in number_of_entries:
            with open(ROOT+"templates/template_"+version+"_graph_prefetch.cfg", "r") as ins:
                config_file = open(ROOT+"config_files/"+version+"/graph_prefetch/"+benchmark+"/"+str(cores)+"/"+ str(entries) + "/" + application+"_"+dataSet+".cfg","w")
                for line in ins:
                    line = line.replace("NUMBER_CORES", str(cores))
                    line = line.replace("STATS_PATH", "zsim_stats/"+version+"/graph_prefetch/"+str(cores)+"/"+ str(entries) + "/" +benchmark+"_"+application+"_"+dataSet)
                    line = line.replace("COMMAND_STRING", "\"" + command + "\";")
                    line = line.replace("WORKLOAD_ROOT",WORKLOAD_ROOT)
                    line = line.replace("PREFETCHER_ENTRIES", str(entries))

                    config_file.write(line)
                config_file.close()
            ins.close()

def create_nuca_graph_prefetch_configs(benchmark, application, dataSet, command, version):
    number_of_cores = [1, 2, 4, 8]
    number_of_entries = [1, 2, 4, 8, 16]
    bank_size = 0.5 * 1024 * 1024 # 0.5MB/bank

    for cores in number_of_cores:
        for entries in number_of_entries:
            mkdir_p(ROOT+"config_files/"+version+"_nuca/graph_prefetch/"+benchmark+"/"+str(cores)+"/"+ str(entries) + "/")

    for cores in number_of_cores:
        for entries in number_of_entries:
            mkdir_p(ROOT+"zsim_stats/"+version+"_nuca/graph_prefetch/"+str(cores)+"/"+ str(entries) + "/")

    for cores in number_of_cores:
        for entries in number_of_entries:
            with open(ROOT+"templates/template_"+version+"_nuca_graph_prefetch.cfg", "r") as ins:
                config_file = open(ROOT+"config_files/"+version+"_nuca/graph_prefetch/"+benchmark+"/"+str(cores)+"/"+ str(entries) + "/" + application+"_"+dataSet+".cfg","w")
                for line in ins:
                    line = line.replace("NUMBER_CORES", str(cores))
                    line = line.replace("LLC_SIZE", str(int(cores * bank_size)))
                    line = line.replace("STATS_PATH", "zsim_stats/"+version+"_nuca/graph_prefetch/"+str(cores)+"/"+ str(entries) + "/" +benchmark+"_"+application+"_"+dataSet)
                    line = line.replace("COMMAND_STRING", "\"" + command + "\";")
                    line = line.replace("WORKLOAD_ROOT",WORKLOAD_ROOT)
                    line = line.replace("PREFETCHER_ENTRIES", str(entries))

                    config_file.write(line)
                config_file.close()
            ins.close()


if(len(sys.argv) < 2):
    print("Usage python generate_config_files.py command_file")
    print("command_file: benckmark,application,dataSet,command,flag(specialized for graph prefetch)")
    exit(1)

with open(sys.argv[1], "r") as command_file:
    for line in command_file:
        line = line.split(",")
        benchmark = line[0]
        application = line[1]
        dataSet = line[2]
        command = line[3]
        flag = line[4]
        flag = flag.replace('\n','')

        print(line)

        if(flag == "graph"):
            # Fixed LLC Size (8MB)
            create_graph_prefetch_configs(benchmark, application, dataSet, command, "inorder")
            create_graph_prefetch_configs(benchmark, application, dataSet, command, "ooo")
            # nuca (0.5MB/bank) mesh
            create_nuca_graph_prefetch_configs(benchmark, application, dataSet, command, "inorder")
            create_nuca_graph_prefetch_configs(benchmark, application, dataSet, command, "ooo")
        else:
            assert(flag == "normal")
            # Fixed LLC Size (8MB)
            create_configs_no_prefetch(benchmark, application, dataSet, command, "inorder")
            create_configs_no_prefetch(benchmark, application, dataSet, command, "ooo")

            create_configs_stream_prefetch(benchmark, application, dataSet, command, "inorder")
            create_configs_stream_prefetch(benchmark, application, dataSet, command, "ooo")
            # nuca (0.5MB/bank) mesh
            create_nuca_configs_no_prefetch(benchmark, application, dataSet, command, "inorder")
            create_nuca_configs_no_prefetch(benchmark, application, dataSet, command, "ooo")

            create_nuca_configs_stream_prefetch(benchmark, application, dataSet, command, "inorder")
            create_nuca_configs_stream_prefetch(benchmark, application, dataSet, command, "ooo")