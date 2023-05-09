#!/bin/bash

sudo modprobe -r kvm-intel
sudo modprobe -r kvm

make -j24 M=arch/x86/kvm
sudo make -j24 INSTALL_MOD_DIR=kernel/arch/x86/kvm M=arch/x86/kvm modules_install

sudo modprobe kvm
sudo modprobe kvm-intel dump_invalid_vmcs=1 pml=0 nested=0
