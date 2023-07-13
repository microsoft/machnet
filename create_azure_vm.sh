#!/bin/bash
#
# Summary: Create an Azure VM with two NICs: one standard, and the other for Machnet with accelerated networking enabled.
#
# See the capitalized parameters below for the configuration

VM_NAME_1=machnet_demo_1
VM_NAME_2=machnet_demo_2

PUBLIC_IP_NAME_SUFFIX="-publicIP"
EXTERNAL_NIC_NAME_SUFFIX="-externalNIC"
MACHNET_NIC_NAME_SUFFIX="-machnetNIC"

IMAGE="Canonical:0001-com-ubuntu-server-jammy:22_04-lts-gen2:latest"
SSH_KEY_FILE="${HOME}/.ssh/id_rsa.pub"
AZURE_USER="$USER"

VNET_NAME="machnet_demo_vnet"
NSG_NAME="machnet_demo_nsg"
SUBNET_NAME="machnet_demo_subnet"
RESOURCE_GROUP="machnet_demo_rg"
ADDRESS_PREFIX="10.0.0.0/21"
VM_SKU="Standard_F8s_v2"
LOCATION="westus"
AZURE_ACCOUNT=$(az account show)

function blue() {
  echo -e "\033[0;36m${1}\033[0m"
}

# Print all parameters and ask y/n
echo -e "Using Azure Account:\n $AZURE_ACCOUNT"
echo -e "\n\n"
echo "Parameters:"
echo "VM_NAMES: $VM_NAME_1 $VM_NAME_2"
echo "IMAGE: $IMAGE"
echo "SSH_KEY_FILE: $SSH_KEY_FILE"
echo "AZURE_USER: $AZURE_USER"
echo "VNET_NAME: $VNET_NAME"
echo "NSG_NAME: $NSG_NAME"
echo "SUBNET_NAME: $SUBNET_NAME"
echo "RESOURCE_GROUP: $RESOURCE_GROUP"
echo "ADDRESS_PREFIX: $ADDRESS_PREFIX"
echo "VM_SKU: $VM_SKU"
echo "LOCATION: $LOCATION"
blue "Continue? (y/n)"
read -r response
if [[ ! $response =~ ^([yY][eE][sS]|[yY])$ ]]; then
  echo "Exiting..."
  exit 1
fi

blue "Have you deleted all previous Azure resources with the "machnet_demo" prefix before running this script? "
blue "This usually requires two runs of find all plus delete in the Azure portal."
blue "Continue? (y/n)"
read -r response
if [[ ! $response =~ ^([yY][eE][sS]|[yY])$ ]]; then
  echo "Exiting..."
  exit 1
fi

# Create resource group if it does not exist
echo "Creating Resource Group $RESOURCE_GROUP..."
if [[ $(az group exists --name $RESOURCE_GROUP) == false ]]; then
  az group create --name $RESOURCE_GROUP --location $LOCATION 1>/dev/null || { echo 'Resource Group creation failed' ; exit 1; }
  echo "Resource Group created successfully."
else
  echo "Resource Group already exists."
fi

# Create a network security group
echo "Creating Network Security Group $NSG_NAME..."
az network nsg create --resource-group $RESOURCE_GROUP --name $NSG_NAME 1>/dev/null || { echo 'Network Security Group creation failed' ; exit 1; }
echo "Network Security Group created successfully."

# Open port 22 for SSH
echo "Opening Port 22 for SSH in $NSG_NAME..."
az network nsg rule create --resource-group $RESOURCE_GROUP --nsg-name $NSG_NAME --name allow_ssh --priority 100 --destination-port-ranges 22 1>/dev/null || { echo 'Failed to open Port 22 for SSH' ; exit 1; }
echo "Port 22 for SSH opened successfully."

# Create a virtual network
echo "Creating vnet $VNET_NAME"
az network vnet create --resource-group $RESOURCE_GROUP --name $VNET_NAME 1>/dev/null || { echo 'Vnet creation failed' ; exit 1; }
echo "Vnet created successfully."

# Create a subnet
echo "Creating subnet $SUBNET_NAME"
az network vnet subnet create --resource-group $RESOURCE_GROUP --vnet-name $VNET_NAME --name $SUBNET_NAME --address-prefixes $ADDRESS_PREFIX 1>/dev/null || { echo 'Subnet creation failed' ; exit 1; }
echo "Subnet created successfully."

# Create external network interfaces
for vm_name in $VM_NAME_1 $VM_NAME_2; do 
  public_ip_name=${vm_name}${PUBLIC_IP_NAME_SUFFIX}
  external_nic_name=${vm_name}${EXTERNAL_NIC_NAME_SUFFIX}
  machnet_nic_name=${vm_name}${MACHNET_NIC_NAME_SUFFIX}

  # Create public IP address
  echo "Creating Public IP for VM $vm_name..."
  az network public-ip create --resource-group $RESOURCE_GROUP --name $public_ip_name 1>/dev/null || { echo 'Public IP creation failed' ; exit 1; }
  echo "Public IP created successfully."

  echo "Creating External NIC for VM $vm_name..."
  az network nic create --resource-group $RESOURCE_GROUP --name $external_nic_name --vnet-name $VNET_NAME --subnet $SUBNET_NAME --public-ip-address $public_ip_name --network-security-group ${NSG_NAME} 1>/dev/null || { echo 'External NIC creation failed' ; exit 1; }
  echo "External NIC created successfully."

  # Create internal network interface with accelerated networking
  echo "Creating Machnet NIC with accelerated networking for VM $vm_name..."
  az network nic create --resource-group $RESOURCE_GROUP --name $machnet_nic_name --vnet-name $VNET_NAME --subnet $SUBNET_NAME --accelerated-networking true 1>/dev/null || { echo 'Internal NIC creation failed' ; exit 1; }
  echo "Internal NIC created successfully."

  # Create the VM
  echo "Creating VM $vm_name..."
  if [ ! -f "$SSH_KEY_FILE" ]; then
    echo "SSH key file $SSH_KEY_FILE does not exist."
    exit 1
  fi

  az vm create --resource-group $RESOURCE_GROUP --name $vm_name --image $IMAGE --size $VM_SKU --nics $external_nic_name $machnet_demo_vnet --admin-username $AZURE_USER --ssh-key-value $SSH_KEY_FILE 1>/dev/null || { echo 'VM creation failed' ; exit 1; }
  blue "VM $vm_name created successfully. Listing SSH address for VM $vm_name:"

  az vm list-ip-addresses --resource-group $RESOURCE_GROUP --name $vm_name --query "[].virtualMachine.network.publicIpAddresses[*].ipAddress" --output tsv || { echo 'Failed to list SSH address for the VM' ; exit 1; }

  echo "" # Add a newline

done
