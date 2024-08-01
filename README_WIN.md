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

1. **Create VM**:

2. **Basics**:
   - **Subscription**: Select your subscription.
   - **Resource group**: Select the same resource group as the VNet.
   - **Virtual machine name**: Enter a name for your VM.
   - **Region**: Ensure it matches the VNet’s region.
   - **Availability options**: Zone 1.
   - **Image**: Windows Server 2019 Datacenter - x64 Gen2.
   - **Size**: Standard_F4s_v2 - 4 vCPUs, 8GiB memory (approx. $237.98/month).

3. **Disks**: Configure as required (leave defaults for basic setup).

4. **Networking**:
   - **Virtual network**: Select the VNet created in Step 1.
   - **Subnet**: Select the default subnet.
   - **Public IP**: Select "None".
   - **NIC network security group**: Choose "Basic".
   - **Inbound ports**: Allow selected ports (RDP (3389)).

5. **Management, Advanced, Tags**: Configure as required (leave defaults for basic setup).

6. **Accelerated Networking**: No.

7. Click "Review + create" and then "Create".

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
   - **Subnet**: Create a new subnet named `AzureBastionSubnet` with a subnet address range of at least /27 (e.g., 10.0.1.0/27).
   - **Public IP address**: Create a new public IP or use an existing one.

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

## Step 5: Log into the VM Using Azure Bastion

1. **Access VM via Bastion**:
   - Navigate to the VM created.
   - Under "Operations", click on "Connect".
   - Select "Bastion".
   - Click "Use Bastion".
   - Enter the username (created during VM setup) and the corresponding private key.
   - Click "Connect".

## Step 6: Install Prerequisites on the Windows VM

1. **RDP into the VM**.

2. **Install Chrome/Edge**.

3. **Install Cygwin**:
   - Download `setup-x86_64.exe`.
   - Select `Git`, `libpkgconf5 2.2.0-1`, `pkg-config 2.2.0-1`, `pkgconf 2.2.0-1` in package selection.

4. **Install LLVM 12.0**:
   - Download from [here](https://releases.llvm.org/download.html).
   - Check "Add LLVM to the system PATH for all users".

5. **Install CMake**:
   - Download from [here](https://cmake.org/download/) (v3.29.3/3.29.6/3.30.0).
   - Check “Add CMake to the system PATH for all users”.

6. **Install Meson 0.57**:
   - From the MSI installer.
   - Download using [this link](https://mesonbuild.com/).

7. **Install Visual Studio 2019 Build Tools**:
   - Use the community edition installer.
   - Under Workloads -> Desktop Development for C++, select only:
     - MSVC v142 - VS 2019.
     - Windows 10 SDK (10.0.19041.0).
   - Under Individual Components Tab:
     - Select MSVC v142 – VS 2019 C++ x64/x86 Spectre-mitigated libs (Latest).

8. **Install Mellanox’s WinOF-2**:
   - Download from [here](https://www.mellanox.com/products/infiniband-drivers/winof-2).

9. **Install Mellanox’s DevX SDK**:
   - Go to `C:\Program Files\Mellanox\MLNX_WinOF2\DevX_SDK`.
   - Install the DevX SDK.

10. **Configure machine for DPDK**:
    - Grant "Lock pages in memory" privilege.
    - Follow instructions from [here](https://doc.dpdk.org/guides/linux_gsg/nic_perf_intel_platform.html).
    - Restart VM.
    - Set up the DevX registry keys.
    - Follow instructions from [here](https://docs.mellanox.com/display/winof2/Setting+Up+Windows+Performance).
    - In addition to the above instructions, set `DevxFsRules` (DWORD) to `00FFFFFF`.
    - In Cygwin, use `mlx5cmd -stat` to see the rules.

## Step 7: Setup virt2phys

1. **Clone the repository**:
   - `git clone git://dpdk.org/dpdk-kmods`.

2. **Check Windows SDK version**: 10.0.19041.685.

3. **Get WDK 10.0.19041.685**:
   - [other-wdk-downloads](https://docs.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk).
   - We want the WDK version that matches the SDK version.
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

1. **Download `dpdk-21.11.tar.xz`** [any other version may cause build errors].
2. **Copy to `cygwin64/home/{vm_name}/`**.
3. **Unzip with 7zip**.
4. **Build locally**:
   - ```
     cd ; cd dpdk ; meson.exe -Dexamples=helloworld build; ninja.exe -C build
     ```
   - Ensure that meson lists `mlx5` as a target under "Drivers enabled". Else there's likely an issue with WinOF2, DevX SDK, or the DevX registry keys.

## Important Hack

- In `dpdk/lib/eal/windows/include/sched.h`, add:
    - ```
    #define RTE_MAX_LCORE 128
    ```
- Without this the machnet build will fail.
## Machnet

1. **Clone the repository**:
 - `git clone https://github.com/microsoft/machnet.git && cd machnet && git checkout t-rrayan/communcation_port`.
 - `git submodule update --init --recursive`.

2. **Download `asio-1.30.2.zip` and unzip in `machnet/third_party/`**.
3. **Change the folder name into `asio`**.
4. **Download `boost_1_82_0.tar.bz2` from [here](https://www.boost.org/users/download/)**.
5. **Copy to `machnet/third_party` and unzip**.
6. **Build**:
 - ```
   rm -rf build && mkdir build && cd build && cmake.exe .. -DCMAKE_BUILD_TYPE=Debug -G Ninja && ninja.exe && cd ..
   ```

## Execution Instructions

1. **Look up the VM’s accelerated NIC’s IP and MAC from NIC’s Resource JSON**:
 - Go to Network Interfaces on Azure Portal.
 - Go to the accelerated NIC.
 - On the NIC’s page, towards top right, click on JSON View.
 - Get the MAC and Private IP from the JSON data.

2. **For now, we hardcode these values in `machnet/src/apps/machnet/config.json`**.
 - If copying MAC from resource JSON, don’t forget to change the “– into “:”.

3. **Set executable permission to `machnet_win.sh`**.

## TODOs

- [ ] Add asio as a submodule.
- [ ] Get rid of asio and use boost.asio.
- [ ] Add boost as a submodule.

## Possible Errors

- **mlx5_net: port 0 failed to set defaults flows**:
- Happens if `DevxFsRules` is set to 8. Change it to `00FFFFFF`. [Source](https://docs.mellanox.com).

## Contact

For questions, please send an email to: rushrukhrayan40@gmail.com