# !/bin/bash

bash scripts/packages/ubuntu18/required.sh

sudo apt-get install build-essential cmake gcc libudev-dev libnl-3-dev libnl-route-3-dev ninja-build pkg-config valgrind python3-dev cython3 python3-docutils pandoc make cmake g++ gcc libnuma-dev libgflags-dev numactl -y

pushd ~

git clone https://github.com/linux-rdma/rdma-core.git
pushd rdma-core && cmake . -DCMAKE_INSTALL_PREFIX:PATH=/usr && sudo make install -j && popd
sudo modprobe ib_uverbs
sudo modprobe mlx4_ib

wget https://fast.dpdk.org/rel/dpdk-19.11.5.tar.xz
tar -xvf dpdk-19.11.5.tar.xz
cp eRPC/common_base dpdk-stable-19.11.5/config/common_base
pushd dpdk-stable-19.11.5/ && sudo make -j install T=x86_64-native-linuxapp-gcc DESTDIR=/usr && popd

popd

sudo bash -c "echo 16384 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages"
sudo bash -c "echo kernel.shmmax = 9223372036854775807 >> /etc/sysctl.conf"
sudo bash -c "echo kernel.shmall = 1152921504606846720 >> /etc/sysctl.conf"
sudo sysctl -p /etc/sysctl.conf

cmake . -DPERF=ON -DTRANSPORT=dpdk -DAZURE=on
