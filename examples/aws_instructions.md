# Instructions to run Machnet on AWS

## Steps to create the VMs

- Create two VMs (e.g., c5.xlarge) using EC2's web interface. This can be done
in one shot by setting "Number of instances" as 2 in the UI, which will also
ensure that the VMs end up in the same subnet.

- Add elastic IPs to each VM to allow SSH access.

- Create two more NICs (one for each VM) in the same subnet as above. These will
be used for private Machnet traffic between the VMs.

  - **Modify these NICs' security group to allow IPv4 traffic.**
  - Associate one NIC to each VM.



## Bind each VM's private NIC to Machnet

The commands below provide a rough outline.
**Before binding the private NIC to DPDK, note down its IP and MAC address.**

```bash
# Install igb_uio
sudo yum install git make kernel-devel
git clone git://dpdk.org/dpdk-kmods
cd dpdk-kmods/linux/igb_uio; make; sudo modprobe uio; sudo insmod igb_uio.ko

# Assuming that ens6 is the private NIC for Machnet
MACHNET_NIC=ens6
cd; wget https://fast.dpdk.org/rel/dpdk-23.11.tar.xz; tar -xvf dpdk-23.11.tar.xz
sudo ifconfig ${MACHNET_NIC} down
sudo ~/dpdk-23.11/usertools/dpdk-devbind.py --bind igb_uio ${MACHNET_NIC}
```