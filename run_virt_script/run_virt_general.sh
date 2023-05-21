#!/bin/bash

#Fill in the following:
#Current user
USER=
#Dir where ubuntu image is at
SOURCE_DIR=$(dirname "$0")
#Guest kernel dir
GUEST_DIR=

# Defaults
MEM_SIZE=8
DEVIRT=
DEVIRT_MACHINE="devirt=off"
NUM_CPUS=16
KPTI_STR=""

# Opts
OPT_HUGE=0
OPT_DEVIRT=0

while getopts ghdm:c: opt; do
    case $opt in
        h)
            OPT_HUGE=1
            ;;
        g)
            OPT_HUGE=2
            ;;
        d)
            OPT_DEVIRT=1
            ;;
        c)
            NUM_CPUS=$((${OPTARG}))
            ;;
        m)
            MEM_SIZE=$((${OPTARG}))
            ;;
    esac
done

# Init for Qemu
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
MEMORY="-object memory-backend-ram,size=${MEM_SIZE}G,merge=off,prealloc=on,id=m0"

# Free old huge pages if there are any
echo 0 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
echo 0 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages

sudo umount /hugepages

if (( $OPT_HUGE != 0 )); then
    MEMORY="-object memory-backend-file,size=${MEM_SIZE}G,merge=off,mem-path=/hugepages,prealloc=on,id=m0"
    sudo mkdir /hugepages
	if (( $OPT_HUGE == 2 )); then
		echo "run huge memory 1G"
		NUM_HUGE=$((${MEM_SIZE}))
		echo $NUM_HUGE | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages
		sudo mount -t hugetlbfs -o pagesize=1G none /hugepages
	else
		echo "run huge memory 2M"
		NUM_HUGE=$(((${MEM_SIZE} * 1024)/2))
		echo $NUM_HUGE | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
		sudo mount -t hugetlbfs -o pagesize=2M none /hugepages
	fi

    sudo chown -R $USER:$USER /hugepages
fi

if (( $OPT_DEVIRT == 1 )); then
    DEVIRT="-device devirt-plain"
    DEVIRT_MACHINE="devirt=on"
    KPTI_STR="pti=on "
    echo "run devirt"
fi

NUM_CPUS_MAX_STR="-$(( $NUM_CPUS - 1 ))"
# Run Qemu
#sudo gdb --args \
sudo numactl --physcpubind 0${NUM_CPUS_MAX_STR} --membind 0 \
qemu-system-x86_64 -s -name debug-threads=on \
-serial stdio -m ${MEM_SIZE}G \
-drive file=$SOURCE_DIR/ubuntu.img,if=virtio,format=qcow2 \
-machine pc,${DEVIRT_MACHINE} \
-enable-kvm -cpu host,migratable=no,+tsc,+tsc-deadline,+rdtscp,+invtsc,+monitor \
$MEMORY \
-device e1000,netdev=net0 \
-netdev user,id=net0,hostfwd=tcp:0.0.0.0:2222-:22 \
-smp ${NUM_CPUS},sockets=1,maxcpus=${NUM_CPUS} \
-numa node,nodeid=0,cpus=0${NUM_CPUS_MAX_STR},memdev=m0 \
-rtc clock=host \
-qmp tcp:localhost:4444,server,nowait \
-vnc localhost:5900 \
-kernel $GUEST_DIR/arch/x86/boot/bzImage -append "nokaslr norandmaps root=/dev/vda1 console=ttyS0 earlyprintk=serial,ttyS0 ignore_loglevel printk_delay=0 systemd.unified_cgroup_hierarchy=1 nopku ${KPTI_STR}" \
$DEVIRT \
