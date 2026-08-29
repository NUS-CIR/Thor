#!/bin/bash

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.
#
# Creates the three Intel E810 VFs used by the ProtoRU setup -- two L1s and the
# middlebox, all on the same PF -- and binds them to vfio-pci for DPDK.
#
# See config/README.md for the resulting VF map.

set -x

NET_INTERFACE=ens6f2np2
sudo ethtool -G $NET_INTERFACE rx 8160
sudo ethtool -G $NET_INTERFACE tx 8160

sudo modprobe iavf

echo 0 | sudo tee /sys/class/net/$NET_INTERFACE/device/sriov_numvfs
echo 3 | sudo tee /sys/class/net/$NET_INTERFACE/device/sriov_numvfs

sudo ip link set $NET_INTERFACE vf 0 mac 00:11:22:33:44:77 vlan 5 qos 0 spoofchk off mtu 9600
sudo ip link set $NET_INTERFACE vf 1 mac 00:11:22:33:44:88 vlan 5 qos 0 spoofchk off mtu 9600
sudo ip link set $NET_INTERFACE vf 2 mac 00:11:22:33:44:99 vlan 5 qos 0 spoofchk off mtu 9600

sudo /usr/local/bin/dpdk-devbind.py --unbind 70:11.0
sudo /usr/local/bin/dpdk-devbind.py --unbind 70:11.1
sudo /usr/local/bin/dpdk-devbind.py --unbind 70:11.2
sudo modprobe vfio-pci
sudo /usr/local/bin/dpdk-devbind.py --bind vfio-pci 70:11.0
sudo /usr/local/bin/dpdk-devbind.py --bind vfio-pci 70:11.1
sudo /usr/local/bin/dpdk-devbind.py --bind vfio-pci 70:11.2
