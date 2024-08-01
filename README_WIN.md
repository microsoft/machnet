# Setup VM on Azure

## Step 1: Create a Virtual Network (VNet)

1. Go to [portal.azure.com](https://portal.azure.com).
2. **Create VNet**:
   - In the left-hand menu, click on "Create a resource".
   - Search for "Virtual Network" and select it.
   - Click "Create".
   
3. **Basics**:
   - **Subscription**: Select your subscription.
   - **Resource group**: Create a new resource group or select an existing one.
   - **Name**: Enter a name for the VNet.
   - **Region**: Select your preferred region.

4. **IP Addresses**:
   - **IPv4 address space**: Use the default address space (e.g., 10.0.0.0/16).
   - **Subnet**: Add a subnet (e.g., name it "default" and use the address range 10.0.0.0/24).

5. **Security**: Leave default.

6. Click "Review + create" and then "Create".

## Step 2: Create the Windows VM without a Public IP

1. **Basics**:
   - **Subscription**: Select your subscription.
   - **Resource group**: Select the same resource group as the VNet.
   - **Virtual machine name**: Enter a name for your VM.
   - **Region**: Ensure it matches the VNet’s region.
   - **Availability options**: Zone 1.
   - **Image**: Windows Server 2019 Datacenter - x64 Gen2.
   - **Size**: Standard_F4s_v2 - 4 vCPUs, 8GiB memory.

2. **Disks**: Configure as required (leave defaults for basic setup).

3. **Networking**:
   - **Virtual network**: Select the VNet created in Step 1.
   - **Subnet**: Select the default subnet.
   - **Public IP**: Select "None".
   - **NIC network security group**: Choose "Basic".
   - **Inbound ports**: Allow selected ports for RDP.

4. **Management, Advanced, Tags**: Configure as required (leave defaults for basic setup).

5. **Accelerated Networking**: No.

6. Click "Review + create" and then "Create".

## Step 3: Create Azure Bastion

1. **Create Bastion**:
   - Navigate to the VNet created in Step 1.
   - Under "Settings", click on "Bastion".
   - Click "Create".

2. **Basics**:
   - **Subscription**: Select your subscription.
   - **Resource group**: Select the same resource group as the VM.
   - **Name**: Enter a name for the Bastion.
   - **Region**: Ensure it matches the VNet’s region.

3. **Configuration**:
   - **Virtual network**: It should be pre-selected.
   - **Subnet**: Create a new subnet named e.g. `AzureBastionSubnet` with a subnet address range of at least /27 (e.g., 10.0.1.0/27).
   - **Public IP address**: Create a new public IP or use an existing one.

4. **NSG Rules for Bastion**:
    - **Inbound Security Rules**:
        - Name: AllowHttpsInbound | Port: 443 | Protocl: TCP | Source: Internet | Destination: Any | Action: Allow
        - Name: AllowGatewayManagerInbound | Port: 443 | Protocl: TCP | Source: GatewayManager | Destination: Any | Action: Allow
        - Name: AllowAzureLoadBalancerInBound | Port: 443 | Protocl: TCP | Source: AzureLoadBalancer | Destination: Any | Action: Allow
        - Name: AllowBastionHostCommunication | Port: 8080, 5701 | Protocl: Any | Source: VirtualNetwork | Destination: VirtualNetwork | Action: Allow
    - **Outbound Security Rules**:
        - Name: AllowSshRdpOutBound | Port: 22, 3389 | Protocl: Any | Source: Any | Destination: VirtualNetwork | Action: Allow
        - Name: AllowAzureCloudOutBound | Port: 443 | Protocl: TCP | Source: Any | Destination: AzureCloud | Action: Allow
        - Name: AllowBastionCommunication | Port: 8080, 5701 | Protocl: Any | Source: VirtualNetwork | Destination: VirtualNetwork | Action: Allow
        - Name: AllowHttpOutBound | Port: 80 | Protocl: Any | Source: Any | Destination: Internet | Action: Allow

    - Check [Azure Docs](https://learn.microsoft.com/en-us/azure/bastion/bastion-nsg) for details.

4. Click "Review + create" and then "Create".

## Step 4: Configure NSG for the VM’s NIC

1. **Create NSG**:
   - Navigate to the "Network security groups" section.
   - Click "Create".

2. **Basics**:
   - **Subscription**: Select your subscription.
   - **Resource group**: Select the same resource group.
   - **Name**: Enter a name for the NSG.
   - **Region**: Ensure it matches your resources.

3. Click "Review + create" and then "Create".

4. **Associate NSG with VM NIC**:
   - Navigate to the VM created in Step 2.
   - Under "Settings", click on "Networking".
   - Select the network interface.
   - Under "Settings", click on "Network security group".
   - Associate the NIC with the NSG created.

5. **NSG Rules**:
   - The default should be fine.

## Step 5: Add accelerated NIC to the VM

1. Create a new NIC with Accelerated Networking Enabled
2. Stop the VM
3. ADD NIC to VM:
   - Go to VM on Portal
   - Network
   - Network Settings
   - Attach network interface

## Step 6: Log into the VM Using Azure Bastion

1. **Access VM via Bastion**:
   - Navigate to the VM created.
   - Under "Operations", click on "Connect".
   - Select "Bastion".
   - Click "Use Bastion".
   - Enter the username (created during VM setup) and the corresponding private key.
   - Click "Connect".

## Step 6: Install Prerequisites on the Windows VM

1. **RDP into the VM**.

2. **Install Cygwin**:
   - Download [`setup-x86_64.exe`](https://www.cygwin.com/install.html).
   - Select `Git`, `libpkgconf5 2.2.0-1`, `pkg-config 2.2.0-1`, `pkgconf 2.2.0-1` in package selection.

3. **Install LLVM 12.0**:
   - Download from [here](https://nam06.safelinks.protection.outlook.com/?url=https%3A%2F%2Fgithub.com%2Fllvm%2Fllvm-project%2Freleases%2Fdownload%2Fllvmorg-12.0.0%2FLLVM-12.0.0-win64.exe&data=05%7C02%7Ct-rrayan%40microsoft.com%7C420a1ea2ea484d38333008dc7c13b50b%7C72f988bf86f141af91ab2d7cd011db47%7C1%7C0%7C638521671304548298%7CUnknown%7CTWFpbGZsb3d8eyJWIjoiMC4wLjAwMDAiLCJQIjoiV2luMzIiLCJBTiI6Ik1haWwiLCJXVCI6Mn0%3D%7C0%7C%7C%7C&sdata=qLvNDJr6CVVcO3fP28oYEOPsKQo91k98PRU8AkhdwJ0%3D&reserved=0).
   - Check "Add LLVM to the system PATH for all users".

4. **Install CMake**:
   - Download from [here](https://nam06.safelinks.protection.outlook.com/?url=https%3A%2F%2Fcmake.org%2Fdownload%2F&data=05%7C02%7Ct-rrayan%40microsoft.com%7C420a1ea2ea484d38333008dc7c13b50b%7C72f988bf86f141af91ab2d7cd011db47%7C1%7C0%7C638521671304557754%7CUnknown%7CTWFpbGZsb3d8eyJWIjoiMC4wLjAwMDAiLCJQIjoiV2luMzIiLCJBTiI6Ik1haWwiLCJXVCI6Mn0%3D%7C0%7C%7C%7C&sdata=UlW6lpkLd67z%2BQJD3KV1rMIstW82WGUJorFGQH%2Byhy8%3D&reserved=0) (v3.29.3/3.29.6/3.30.0).
   - Check “Add CMake to the system PATH for all users”.

5. **Install Meson 0.57**:
   - From the MSI installer.
   - Download using [this link](https://nam06.safelinks.protection.outlook.com/?url=https%3A%2F%2Fgithub.com%2Fmesonbuild%2Fmeson%2Freleases%2Fdownload%2F0.57.0%2Fmeson-0.57.0-64.msi&data=05%7C02%7Ct-rrayan%40microsoft.com%7C420a1ea2ea484d38333008dc7c13b50b%7C72f988bf86f141af91ab2d7cd011db47%7C1%7C0%7C638521671304566032%7CUnknown%7CTWFpbGZsb3d8eyJWIjoiMC4wLjAwMDAiLCJQIjoiV2luMzIiLCJBTiI6Ik1haWwiLCJXVCI6Mn0%3D%7C0%7C%7C%7C&sdata=O9Sdbt4w6m%2FELsesFYBdRyNceVOHMIZ6RyIUfXR4q%2Fs%3D&reserved=0).

6. **Install Visual Studio 2019 Build Tools**:
   - Use the community edition installer.
   - Under Workloads -> Desktop Development for C++, select only:
     - MSVC v142 - VS 2019.
     - Windows 10 SDK (10.0.19041.0).
   - Under Individual Components Tab:
     - Select MSVC v142 – VS 2019 C++ x64/x86 Spectre-mitigated libs (Latest).

7. **Install Mellanox’s WinOF-2**:
   - Download from [here](https://www.mellanox.com/products/adapter-software/ethernet/windows/winof-2).

8. **Install Mellanox’s DevX SDK**:
   - Go to `C:\Program Files\Mellanox\MLNX_WinOF2\DevX_SDK`.
   - Install the DevX SDK.

9. **Configure machine for DPDK**:
    - **Grant "Lock pages in memory" privilege.**
        - Follow instructions from [DPDK Get Started from Windows](https://doc.dpdk.org/guides/windows_gsg/run_apps.html#grant-lock-pages-in-memory-privilege).
        - Restart VM.
    - **Set up the DevX registry keys.**
        - Follow instructions from [Nvidia WinOF Driver Docs](https://docs.nvidia.com/networking/display/winof2v290/devx+interface).
        - In addition to the above instructions, set `DevxFsRules` (DWORD) to `00FFFFFF`.
        - In Cygwin, use `mlx5cmd -stat` to see the rules.

## Step 7: Setup virt2phys [Optional: Not needed with mlx5_pmd driver with Nvidia CX NICs]

1. **Clone the repository**:
   - `git clone git://dpdk.org/dpdk-kmods`.

2. **Check Windows SDK version**: 10.0.19041.685.
   - To find the right SDK version, just see control panel/uninstall program/windows software development kit

3. **Get WDK 10.0.19041.685**:
   - [other-wdk-downloads](https://learn.microsoft.com/en-us/windows-hardware/drivers/other-wdk-downloads).
   - We want the WDK version that matches the SDK version.
   - Version 19041 is incorrectly linked to Windows 10, Version 2004 WDK for Windows 10, version 2004
   - When installation finishes, opt for “Install Windows Driver Kit Visual Studio Extension”.

4. **Disable Signature Enforcement**:
   - Open Command Prompt as administrator.
   - Execute the following commands:
     ```
     bcdedit -set loadoptions DISABLE_INTEGRITY_CHECKS
     bcdedit -set TESTSIGNING ON
     shutdown -r -t 0
     ```
   - This will prompt a restart to the VM.
   - Upon restart, confirm if the boot is on Test Mode (bottom right on desktop).

5. **Build virt2phys**:
   - Go to `C:\cygwin64\home\{your-vm-name}\dpdk-kmods\windows\virt2phys`.
   - Open `virt2phys.sln` file using VS 2019.
   - In Solution Explorer > `virt2phys` > properties > Driver Signing > General > Sign Mode: Set to Off.
   - Select Debug Build.
   - If you see Error: MSB8040 Spectre-mitigated libraries are required for this project, check VS setup step above.

6. **Install virt2phys**:
   - Open Command prompt and cd `C:\cygwin64\home\{your-vm-name}\dpdk-kmods\windows\virt2phys`.
   - Run the following:
     ```
     pnputil /add-driver x64\Debug\virt2phys\virt2phys.inf /install
     ```
   - Check if the driver is added using the following:
     ```
     pnputil /enum-drivers
     ```

7. **Additional steps for Windows Server**:
   - Device Manager > Action > Add Legacy Hardware.
   - Select “Install HW I manually select from a list”.
   - Select Kernel Bypass from the list.

## Step 8: Build DPDK

1. **Download [`dpdk-21.11.tar.xz`](https://fast.dpdk.org/rel/)** [any other version may cause build errors].
2. **Copy to `cygwin64/home/{vm_name}/`**.
3. **Unzip with 7zip**.
4. **Build locally**:
   - ```
     cd ; cd dpdk ; meson.exe -Dexamples=helloworld build; ninja.exe -C build
     ```
   - Ensure that meson lists `mlx5` as a target under "Drivers enabled". Else there's likely an issue with WinOF2, DevX SDK, or the DevX registry keys.

## Important Hack

    - In `dpdk/lib/eal/windows/include/sched.h`, add: `#define RTE_MAX_LCORE 128`
    - Without this the machnet build may fail.

## Machnet

1. Clone the repository:
    ```
    git clone https://github.com/microsoft/machnet.git && cd machnet && git checkout t-rrayan/communcation_port
    git submodule update --init --recursive
    ```

2. Download `asio-1.30.2.zip` and unzip in `machnet/third_party/`.
3. Change the folder name into `asio`.
4. Download `boost_1_82_0.tar.bz2` from [here](https://www.boost.org/users/download/).
5. Copy to `machnet/third_party/` and unzip.
6. Build from `machnet/`:
    ```
    rm -rf build && mkdir build && cd build && cmake.exe .. -DCMAKE_BUILD_TYPE=Release -G Ninja && ninja.exe && cd ..
    ```

## Execution Instructions

1. **Look up the VM’s accelerated NIC’s IP and MAC from NIC’s Resource JSON**:
    - Go to Network Interfaces on Azure Portal.
    - Go to the accelerated NIC.
    - On the NIC’s page on the portal, towards top right, click on JSON View.
    - Get the MAC and Private IP from the JSON data.

2. **For now, we hardcode these values in `machnet/src/apps/machnet/config.json`**.
    - see `src/apps/machnet/config.json`.

3. **Set executable permission to `machnet_win.sh`**.

4. **Repeat everything up until here for the second VM**

5. **Run Hello World**
   - **VM1 - Server**:
        - Launch the controller from machnet root with: `./machnet_win.sh`
        - Launch the app: `./build/bin/hello_world.exe --local_ip [VM1 acc. NIC's IP]`
   - **VM2 - Client**:
        - Launch the controller from machnet root with: `./machnet_win.sh`
        - Launch the app: `./build/bin/hello_world.exe --local_ip [VM2 acc. NIC's IP] --remote_ip [VM1 acc. NIC's IP]`

5. **Run RTT Latency Benchmark**
   - **VM1 - Server**:
        - Launch the controller from machnet root with: `./machnet_win.sh`
        - Launch the app: `./build/bin/msg_gen.exe --local_ip [VM1 acc. NIC's IP]`
   - **VM2 - Client**:
        - Launch the controller from machnet root with: `./machnet_win.sh`
        - Launch the app: `./build/bin/msg_gen.exe --local_ip [VM2 acc. NIC's IP] --remote_ip [VM1 acc. NIC's IP] --msg_window 1 --msg_size 1024`

## TODOs

- Add asio as a submodule.
- Or, get rid of asio and use boost.asio.
- Add boost as a submodule.
- In `src/include/arp.h`, `GetL2AddrWin`:
    - now uses ping to remote and windows arp cache to resolve remote physical address.
    - `mlx5` driver on Windows may have issue in handling ARP requests.
    - may use a better solution.
- Only `apps/machnet`, `hello_world` and `msg_gen` are ported to Windows.
- No tests/benchmarks/bindings have been updated.
- `src/ext/machnet.c` is now `src/ext/machnet.cc`: may impact `bindings`.
- Linux execution of this src has not been tested, may need minor changes.

## Possible Errors

- **mlx5_net: port 0 failed to set defaults flows**:
    - Happens if `DevxFsRules` is set to 8. Change it to `00FFFFFF`. 
    - Details: [here](https://forums.developer.nvidia.com/t/dpdk-devx-register-failed/219774).

## Contact

For questions, contact at: rushrukhrayan40@gmail.com