#!/bin/bash

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.
#
# Creates the Intel E810 VFs that the L1s and the middlebox use, and binds them
# to vfio-pci for DPDK. Three VFs per PF, all on VLAN 5 with a 9600-byte MTU.
#
# The middlebox port itself (0000:43:00.3, a Mellanox CX-7 VF) is NOT handled
# here: mlx5 VFs are driven through the mlx5 PMD and stay bound to mlx5_core.
#
# See config/README.md for the resulting VF map.

set -x

NET_INTERFACE=ens6f1np1
sudo ethtool -G $NET_INTERFACE rx 8160
sudo ethtool -G $NET_INTERFACE tx 8160

sudo modprobe iavf

echo 0 | sudo tee /sys/class/net/$NET_INTERFACE/device/sriov_numvfs
echo 3 | sudo tee /sys/class/net/$NET_INTERFACE/device/sriov_numvfs

sudo ip link set $NET_INTERFACE vf 0 mac 00:11:22:33:44:55 vlan 5 qos 0 spoofchk off trust on mtu 9600
sudo ip link set $NET_INTERFACE vf 1 mac 00:11:22:33:44:56 vlan 5 qos 0 spoofchk off trust on mtu 9600
sudo ip link set $NET_INTERFACE vf 2 mac 00:11:22:33:44:57 vlan 5 qos 0 spoofchk off trust on mtu 9600

sudo /usr/local/bin/dpdk-devbind.py --unbind 70:09.0
sudo /usr/local/bin/dpdk-devbind.py --unbind 70:09.1
sudo /usr/local/bin/dpdk-devbind.py --unbind 70:09.2
sudo modprobe vfio-pci
sudo /usr/local/bin/dpdk-devbind.py --bind vfio-pci 70:09.0
sudo /usr/local/bin/dpdk-devbind.py --bind vfio-pci 70:09.1
sudo /usr/local/bin/dpdk-devbind.py --bind vfio-pci 70:09.2

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

NET_INTERFACE=ens6f3np3
sudo ethtool -G $NET_INTERFACE rx 8160
sudo ethtool -G $NET_INTERFACE tx 8160

sudo modprobe iavf
echo 0 | sudo tee /sys/class/net/$NET_INTERFACE/device/sriov_numvfs
echo 3 | sudo tee /sys/class/net/$NET_INTERFACE/device/sriov_numvfs
sudo ip link set $NET_INTERFACE vf 0 mac 00:11:22:33:44:AA vlan 5 qos 0 spoofchk off trust on mtu 9600
sudo ip link set $NET_INTERFACE vf 1 mac 00:11:22:33:44:AB vlan 5 qos 0 spoofchk off trust on mtu 9600
sudo ip link set $NET_INTERFACE vf 2 mac 00:11:22:33:44:AC vlan 5 qos 0 spoofchk off trust on mtu 9600

sudo /usr/local/bin/dpdk-devbind.py --unbind 70:19.0
sudo /usr/local/bin/dpdk-devbind.py --unbind 70:19.1
sudo /usr/local/bin/dpdk-devbind.py --unbind 70:19.2
sudo modprobe vfio-pci
sudo /usr/local/bin/dpdk-devbind.py --bind vfio-pci 70:19.0
sudo /usr/local/bin/dpdk-devbind.py --bind vfio-pci 70:19.1
sudo /usr/local/bin/dpdk-devbind.py --bind vfio-pci 70:19.2