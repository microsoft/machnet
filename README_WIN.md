# Setup VM on Azure:  

Step 1: Create a Virtual Network (VNet) 

Go to portal.azure.com. 

Create VNet:  

In the left-hand menu, click on "Create a resource". 

Search for "Virtual Network" and select it. 

Click "Create". 

Basics: 

Subscription: Select your subscription. 

Resource group: Create a new resource group or select an existing one. 

Name: Enter a name for the VNet. 

Region: Select your preferred region. 

IP Addresses: 

IPv4 address space: You can use the default address space (e.g., 10.0.0.0/16). 

Subnet: Add a subnet (e.g., name it "default" and use address range 10.0.0.0/24) 

Security: Leave default. 

Click "Review + create" and then "Create". 

 

Step 2: Create the Windows VM without a Public IP 

Create VM: 

Basics: 

Subscription: Select your subscription. 

Resource group: Select the same resource group as the VNet. 

Virtual machine name: Enter a name for your VM. 

Region: Ensure it matches the VNet’s region. 

Availability options: Zon 1 

Image: Windows Server 2019 Datacenter - x64 Gen2 

Size: Standard_F4s_v2 - 4 vcpus, 8GiB memory (237.98/month) 

Disks: Configure as required (leave defaults for basic setup). 

Networking: 

Virtual network: Select the VNet created in Step 1. 

Subnet: Select the default subnet. 

Public IP: Select "None". 

NIC network security group: Choose "Basic". 

Inbound ports: Allow selected ports (RDP (3389)). 

Management, Advanced, Tags: Configure as required (leave defaults for basic setup). 

Accelerated Networking: No 

Click "Review + create" and then "Create". 

Step 3: Create Azure Bastion 

Create Bastion: 

Navigate to the VNet created in Step 1. 

Under "Settings", click on "Bastion". 

Click "Create". 

Basics: 

Subscription: Select your subscription. 

Resource group: Select the same resource group as the VM. 

Name: Enter a name for the Bastion. 

Region: Ensure it matches the VNet’s region. 

Configuration: 

Virtual network: It should be pre-selected. 

Subnet: Create a new subnet named AzureBastionSubnet with a subnet address range of at least /27 (e.g., 10.0.1.0/27). 

Public IP address: Create a new public IP or use an existing one. 

Click "Review + create" and then "Create". 

 

NSG Rules for Bastion: 

A screenshot of a computer 

 

Step 4: Configure NSG for the VM’s NIC 

Create NSG: 

Navigate to the "Network security groups" section. 

Click "Create". 

Basics: 

Subscription: Select your subscription. 

Resource group: Select the same resource group. 

Name: Enter a name for the NSG. 

Region: Ensure it matches your resources. 

Click "Review + create" and then "Create". 

 

 

 

 

 

 

 

 

 

NSG Rules: 

A screenshot of a computer

Description automatically generated 

Associate NSG with VM NIC: 

Navigate to the VM created in Step 2. 

Under "Settings", click on "Networking". 

Select the network interface. 

Under "Settings", click on "Network security group". 

Associate the NIC with the NSG created. 

 

Step 5: Log into the VM Using Azure Bastion 

Access VM via Bastion: 

Navigate to the VM created. 

Under "Operations", click on "Connect". 

Select "Bastion". 

Click "Use Bastion". 

Enter the username (created during VM setup) and the corresponding private key. 

Click "Connect". 

 

 

Step 6:  

create an accelerated NIC 

attach to VM 

apply NSG to both NICs 

 

Install prerequisites on the Windows VM 

RDP into the VM 

Install chrome/edge: Follow this. 

Install Cygwin: 

Download setup-x86_64.exe 

Select Git, libpkgconf5 2.2.0-1, pkg-config 2.2.0-1, pkgconf 2.2.0-1 in package selection 

Install LLVM 12.0 

Download from here 

Check "Add LLVM to the system PATH for all users" 

Install CMake 

Download from here (v3.29.3/3.29.6/3.30.0) 

Check “Add CMake to the system PATH for all users” 

Install meson 0.57 

from the MSI installer 

Download using this	 

Install Visual Studio 2019 build tools: 

Use the community edition installer 

Under Workloads -> Desktop Development for C++, select only: 

MSVC v142 - VS 2019 

Windows 10 SDK (10.0.19041.0) 

Under Individual Components Tab: 

Select MSVC v142 – VS 2019 C++ x64/x86 Spectre-mitigated libs (Latest) 

Otherwise later virt2phys build breaks in VS. 

 

Install Mellanox’s WinOF-2  

Download from here 

Install Mellanox’s DevX SDK 

Go to C:\Program Files\Mellanox\MLNX_WinOF2\DevX_SDK 

Install the DevX SDK 

 

Configure machine for DPDK 

Grant "Lock pages in memory" privilege 

Follow instructions from here 

Restart VM 

Set up the DevX registry keys 

Follow instructions from here 

In addition to above instructions, set DevxFsRules (DWORD) to 00FFFFFF 

In Cygwin, use mlx5cmd -stat to see the rules. 

Note: these keys may need resetting if the VM is restarted. 

 

Setup virt2phys: [for more info: dpdk_win_setup, dpdk/win/readme] [optional – not needed with mlx5_pci driver] 

git clone git://dpdk.org/dpdk-kmods 

Check Windows SDK version: 10.0.19041.685 

To find the right SDK version, just see control panel/uninstall program/windows software development kit 

Get WDK 10.0.19041.685: 

other-wdk-downloads 

We want the WDK version that matches the SDK version 

Version 19041 is incorrectly linked to Windows 10, Version 2004 WDK for Windows 10, version 2004 [direct download link - this] 

When installation finishes, opt for “Install Windows Driver Kit Visual Studio Extension” 

Disable Signature Enforcement: 

Open Command Prompt as administrator 

Execute the following commands: 

bcdedit -set loadoptions DISABLE_INTEGRITY_CHECKS 

bcdedit -set TESTSIGNING ON 

shutdown -r -t 0 

This will prompt a restart to the VM. 

Upon restart, confirm if the boot is on Test Mode (bottom right on desktop) 

Build virt2phys: 

Go to C:\cygwin64\home\{your-vm-name}\dpdk-kmods\windows\virt2phys 

Open virt2phys.sln file using VS 2019 

Solution Explorer > vist2phys > properties > Driver Signing > General > Sign Mode: Set to Off. 

Select Debug Build. 

If you see Error: MSB8040 Spectre-mitigated libraries are required for this project, check VS setup step above. 

Install virt2phys: 

Open Command prompt and cd C:\cygwin64\home\{your-vm-name}\dpdk-kmods\windows\virt2phys 

Run the following: 

pnputil /add-driver x64\Debug\virt2phys\virt2phys.inf /install 

Check if the driver is added using the following: 

pnputil /enum-drivers 

Additional steps for Windows Server: 

Device Manager > Action > Add Legacy Hardware 

Select “Install HW I manually select from a list” 

Select Kernel Bypass from the list 

 

Build DPDK 

Download dpdk-21.11.tar.xz [any other version may cause build errors] 

Copy to cygwin64/home/vm_name/ 

Unzip with 7zip 

Build locally:  
cd ; cd dpdk ; meson.exe -Dexamples=helloworld build; ninja.exe -C build 

Ensure that meson lists mlx5 as a target under "Drivers enabled". Else there's likely an issue with WinOF2, DevX SDK, or the DevX registry keys 

 
[Important hack] add the following: 

In dpdk/lib/eal/windows/include/sched.h: 

#define RTE_MAX_LCORE 128 

Without this the machnet build will fail. 

 

Machnet: 

 

git clone https://github.com/microsoft/machnet.git && cd machnet && git checkout t-rrayan/communcation_port; 

git submodule update --init –recursive 

download asio-1.30.2.zip and unzip in machnet/third_party/ 

change the folder name into asio. 

Download boost_1_82_0.tar.bz2 from here 

Copy to machnet/third_party and unzip. 

Execute the following from the project root: 

rm -rf build && mkdir build && cd build && cmake.exe .. -DCMAKE_BUILD_TYPE=Debug -G Ninja && ninja.exe && cd .. 

Execution Instructions: 

Look up the VM’s accelerated NIC’s IP and MAC from NIC’s Resource JSON. 

Go to Network Interfaces on Azure Portal 

Go to the accelerated NIC 

On the NIC’s page, towards top right, click on JSON View 

Get the MAC and Private IP from from the JSON data 

 

 

For now we hardcode these values in machnet/src/apps/machnet/config.json 

 

If copying MAC from resource JSON, don’t forget to change the “– into “:”. 

Set executable permission to machnet_win.sh 

 

If possible: 

[to-do] add asio as a submodule 

[to-do] get rid of asio and use boost.asio 

[to-do] add boost as a submodule 

 

 

Possible errors: 

mlx5_net: port 0 failed to set defaults flows 

happends DevxFsRules is set to 8. Change it into 00FFFFFF. 

Source - here 

 

Contact: 

For questions, please send email to: rushrukhrayan40@gmail.com 

 