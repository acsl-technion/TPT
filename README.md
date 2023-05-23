# TPT

This is the kernel code for "Translation Pass-Through for Near-Native Paging Performance in VMs", Usenix ATC 23. Tested with Ubuntu 20.04.

This source-code is POC and given as-is. Code structure can be improved, otherwise it is entirely functional. Any suggestions are more than welcome!

## Components Overview
* **guest_kernel**: The TPT guest kernel (with the TPT pv-ops backend).
* **host_kernel**: The TPT hypervisor kernel. This is a plain vanilla kernel with a modified KVM module that supports TPT and its emulation on Intel x86_64.
* **qemu**: TPT supporting QEMU.
* **run_virt_script**: collection of scripts to deploy a TPT/non-TPT VM.

## Quick Setup:

### Dependencies:

For Linux kernel compilation:

https://wiki.ubuntu.com/Kernel/BuildYourOwnKernel

For QEMU compilation

https://wiki.qemu.org/Hosts/Linux

In each of their respective folders:

### Host Kernel
```bash
make tpt_defconfig
make -j$(nproc)
sudo make modules_install
sudo make install
```

### Guest Kernel
```bash
make tpt_defconfig
make -j$(nproc)
```

### QEMU
```bash
mkdir build && cd build
../configure --target-list=x86_64-softmmu  
make -j$(nproc)
sudo make install
```

## Quick Run:
Modify the variables in the script:

run_virt_script/run_virt_general.sh.

And specify the current user and the directory of the kernel and the Ubuntu image.

Run the script to spawn a VM:

`./run_virt_general.sh -d`

`-d` flag specifies that the hypervisor exposes TPT

SSH in to the virtual machine.

Run:

`echo 1 | sudo tee /sys/kernel/devirt`

To fault in all VM pages and map them into EPT to minimize inconsistency between runs.

To spawn a process with TPT, enter its name into `/sys/kernel/mm/devirt/task_name`

For example:

`echo sysbench | sudo tee /sys/kernel/mm/devirt/task_name`

Then run the process.

### Toy example to show the benefits of TPT with sysbench for random memory accesses:

Spawn a TPT capable VM with `./run_virt_general.sh -d`

SSH into the machine:

```bash
tpt@tpt:~$ echo 1 | sudo tee /sys/kernel/devirt
1

tpt@tpt:~$ # Run without TPT
tpt@tpt:~$ sysbench memory --memory-block-size=4G --memory-access-mode=rnd run
sysbench 1.0.18 (using system LuaJIT 2.1.0-beta3)
...
General statistics:  
    total time:                          `31.3870s`
    
tpt@tpt:~$ # Enable TPT for Sysbench
tpt@tpt:~$ echo sysbench | sudo tee /sys/kernel/mm/devirt/task_name
tpt@tpt:~$ sysbench memory --memory-block-size=4G --memory-access-mode=rnd run
sysbench 1.0.18 (using system LuaJIT 2.1.0-beta3)
...
General statistics:
   total time:                          `14.9101s`
```

Results with TPT equal to that of native execution.
