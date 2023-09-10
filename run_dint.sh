#!/usr/bin/python3

import subprocess
import os
from time import sleep
import sys

erpc_dir_path = '/users/yangzhou/eRPC'
num_cores = 8

servers = ['clnode282.clemson.cloudlab.us']
clients = ["clnode273.clemson.cloudlab.us",
           "clnode255.clemson.cloudlab.us", "clnode276.clemson.cloudlab.us"]


def kill_all():
    for machine in servers + clients:
        sleep(0.1)
        subprocess.Popen(['ssh', '-o', 'StrictHostKeyChecking=no', '-i', '~/.ssh/id_rsa', machine, "sudo pkill small_rpc"], shell=False,
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    sleep(5)


def prepare_binaries():
    executors = []
    for machine in clients:
        e = subprocess.Popen(['rsync', '-auv', '--exclude=.git/', f'{erpc_dir_path}/', f'{machine}:{erpc_dir_path}'],
                             shell=False,
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        executors.append(e)
    for e in executors:
        out, err = e.communicate()

    executors = []
    for machine in servers + clients:
        sleep(0.1)
        e = subprocess.Popen(['ssh', '-o', 'StrictHostKeyChecking=no', '-i', '~/.ssh/id_rsa', machine, f"cd {erpc_dir_path} && export RTE_SDK=~/dpdk && cmake . -DPERF=ON -DTRANSPORT=dpdk -DAZURE=on && make clean && make -j small_rpc_tput"],
                             shell=False,
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        executors.append(e)
    for e in executors:
        out, err = e.communicate()


def run_erpc_expr(app, num_of_uthreads):
    msg_size = 40
    num_processes = len(servers) + len(clients)

    kill_all()

    executors = []
    print("start server")
    e = subprocess.Popen(['ssh', '-o', 'StrictHostKeyChecking=no', '-i', '~/.ssh/id_rsa', servers[0],
                          f"cd {erpc_dir_path} && autorun_app=small_rpc_tput sudo ./build/small_rpc_tput --test_ms 20000 --sm_verbose 0 --num_processes {num_processes} --numa_0_ports 2 --numa_1_ports 2 --numa_node=1 --process_id=0 --num_threads {num_cores} --is_client 0 &> /dev/null"],
                         shell=False,
                         stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE
                         )
    # For for hashtable initing
    sleep(22)

    print("start clients")
    for i, client in enumerate(clients):
        sleep(0.1)
        e = subprocess.Popen(['ssh', '-o', 'StrictHostKeyChecking=no', '-i', '~/.ssh/id_rsa', client,
                              f"cd {erpc_dir_path} && autorun_app=small_rpc_tput sudo stdbuf -o0 ./build/small_rpc_tput --test_ms 20000 --sm_verbose 0 --num_processes {num_processes} --numa_0_ports 2 --numa_1_ports 2 --numa_node=1 --process_id={i + 1} --num_threads 64 --num_clients {num_of_uthreads} --num_dst_threads {num_cores} --is_client 1"],
                             shell=False,
                             stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        executors.append(e)

    print("get output")
    outputs = []
    for e in executors:
        out, err = e.communicate()
        out = out.decode('utf-8').splitlines()[-20:]
        outputs.append(out)
        err = err.decode('utf-8').splitlines()[-20:]
        outputs.append(err)

    # write output
    with open(f"results/{app}_bench_parallel_keys_all_erpc_nu_{num_of_uthreads}.txt", "w") as f:
        for i in range(len(outputs)):
            f.write(f'result {i}:\n')
            for line in outputs[i]:
                f.write(line + '\n')

    kill_all()


if __name__ == "__main__":
    if (len(sys.argv) < 2):
        print("Usage: ./run_dint.sh [command]")
        exit(1)

    if (sys.argv[1] == "binary"):
        prepare_binaries()
    elif (sys.argv[1] == "run"):
        if (len(sys.argv) != 4):
            print(
                "Usage: ./run_dint.sh run [store|lock_fasst] [#uthreads_per_machine]")
            exit(1)

        app = sys.argv[2]
        nu = int(sys.argv[3])
        run_erpc_expr(app, nu)
    else:
        print("unknown command")
        exit(1)
