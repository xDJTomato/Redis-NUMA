1. PREREQUISITE
1) G++ (GNU C++ Compiler)
2) libnuma library
3) root privileges (sudo)

2. INSTALLATION
1) Samsung C-TAP supports Linux only
2) Copy the C-TAP binary to any directory on your system and root privileges are required to run this tool

3. COMMAND LINE ARGUMENTS
Usage: sudo ./ctap [command] [options]

COMMANDS
    -v               : --version                     | print version
    -h               : --help                        | print this help
    -I               : --system_info                 | show system information (node, memory devices)
    -B               : --bandwidth                   | measure bandwidth
    -Y               : --burst_latency               | measure burst latency (prefer sudo)
    -Z               : --loaded_latency              | measure loaded latency (prefer sudo)
    -T               : --bandwidth_local             | measure bandwidth on local cpu-node

OPTIONS
    -m <size>        : --mem_size=<size>             | memory size[byte]. (g/G: GB, m/M: MB, k/K: KB)
    -o <filename>    : --output=<filename>           | log filename
    -c <chunk>       : --chunk=<chunk>               | chunk size. Default=1 (16byte)
    -C <node>        : --cpunode=<node>              | bind cpu-node
    -M <node>        : --memnode=<node>              | bind memory-node
    -V <verbose>     : --verbose=<verbose>           | 1: save log file, 0: only print on terminal. Default=1
    -s <size>        : --stride=<size>               | stride size[byte]. Default=64
    -i <iter>        : --iter=<iter>                 | test iterations.
    -k <cpu_id>      : --latency_cpu=<cpu_id>        | use cpu binding to measure latency.
    -l <cpu_id_list> : --load_cpu=<cpu_id_list>      | use cpu binding to meausre bandwidth. (ex. -l1-2,5-8)
    -A               : --auto                        | test all memory devices w/o DIMM. Default=False
    -f <path>        : --devdax=<path>               | use devdax mode. Only latency mode
    -g <path>        : --pmem=<path>                 | use persistent memory

BW CONFIGS
    -a               : --basic_workload              | Test each workloads: 1R, 1W(non-temporal write),
                                                     | 3R1W, 2R1W, 1R1W, 2R1W(non-temporal write)
    -r <ratio>       : --read=<ratio>                | read ratio
    -w <ratio>       : --write=<ratio>               | write ratio
    -n               : --stream                      | use non-temporal write (bypass cache)
    -j <number>      : --thread=<number>             | the number of threads. Min=2 (Main thread use 1 thread)
    -F               : --full                        | use full(64byte) load/store instruction.
                                                     | Default=False (16byte)
    -D <delay>       : --bw_delay=<delay>            | utilize bandwidth (injection delay in load/store)
                                                     | measure loaded_latency at specific delay

If not use sudo, C-TAP can't disable prefetcher. Then, C-TAP can't measure sequential latency.
So, It is recommended to give root privileges.
<chunk> is 0 (8byte), 1 (16byte) or 2 (32byte)
If system doesn't support 16byte instructions, C-TAP will generate 8 byte chunk-size
chunk config supports only latency mode.
<size> can have k/K (KiB), m/M (MiB), g/G (GiB)
'-A' option can't use with '-C' and '-M' options
'-C, -T, -j' option can't use with '-l' and '-k' options

example) sudo ./ctap --burst_latency -C0 -M0


========================================================================================
Copyright 2024. SAMSUNG Electronics Co., Ltd. all rights reserved.
Samsung C-TAP(CMM Test Agent Platform) - Version 2.2.4, Dec. 2025

Author
- Jinyoung Moon (jy323.moon@samsung.com)
- Seungwoo Lim (sws.lim@samsung.com)
- Kyumin Park (kyumin.park@samsung.com)
- SeungPyo Cho (sp82.cho@samsung.com)
========================================================================================
