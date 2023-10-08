#!/usr/bin/env python3
import os
import argparse
from termcolor import cprint

from azure.identity import AzureCliCredential
from azure.mgmt.compute import ComputeManagementClient
from azure.mgmt.network import NetworkManagementClient
from azure.mgmt.resource import ResourceManagementClient
from azure.core.exceptions import ResourceNotFoundError

AZURE_SUBSCRIPTION_ID_ENV = "AZURE_SUBSCRIPTION_ID"
MACHNET_ADDRESS_SPACE = "10.0.0.0/16"

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--resource_group", type=str, required=True, help="Resource group name to create/use")
    parser.add_argument("--location", type=str, required=True, help="Azure location to use")
    parser.add_argument("--nickname", type=str, required=True, help="Nickname to use as a prefix for Machnet resources")
    parser.add_argument("--num_vms", type=int, default=1, help="Number of VMs to create")
    args = parser.parse_args()

    if not args.nickname.isalnum():
        cprint("Error: nickname must be alphanumeric", "red")
        exit()

    credential = AzureCliCredential()
    if AZURE_SUBSCRIPTION_ID_ENV not in os.environ:
        cprint(f"Error: Please set environment variable {AZURE_SUBSCRIPTION_ID_ENV}", "red")
        exit()

    c_subscription_id = os.environ[AZURE_SUBSCRIPTION_ID_ENV]
    cprint(f"Using Azure subscription ID = {c_subscription_id}")

    ## Resource group
    resource_client = ResourceManagementClient(credential, c_subscription_id)
    print(f"Provisioning resource group {args.resource_group} in {args.location}.") 
    rg_result = resource_client.resource_groups.create_or_update(args.resource_group, {"location": args.location})
    cprint(f"Provisioned resource group {rg_result.name} in the {rg_result.location} region", "green")

    # Resources will be named .nickname}-{location}-XXX
    c_vnet_name = f"{args.nickname}-{args.location}-vnet"
    c_subnet_name = f"{args.nickname}-{args.location}-subnet"
    c_ip_name = f"{args.nickname}-{args.location}-ip"
    c_ip_config_name = f"{args.nickname}-{args.location}-ip-config"
    c_public_nic_name = f"{args.nickname}-{args.location}-public-nic"
    c_machnet_nic_name = f"{args.nickname}-{args.location}-machnet-nic"
    c_vm_names = [f"{args.nickname}-{args.location}-vm-{i}" for i in range(args.num_vms)]

    ## Vnet
    network_client = NetworkManagementClient(credential, c_subscription_id)
    print(f"Provisioning vnet {c_vnet_name} with prefix {MACHNET_ADDRESS_SPACE}")
    try:
        existing_vnet = network_client.virtual_networks.get(args.resource_group, c_vnet_name)
    except ResourceNotFoundError:
        print(f"  Vnet {c_vnet_name} does not exist")
        existing_vnet = None

    if existing_vnet is None:
        poller = network_client.virtual_networks.begin_create_or_update(
            args.resource_group,
            c_vnet_name,
            {
                "location": args.location,
                "address_space": {"address_prefixes": [MACHNET_ADDRESS_SPACE]},
            },
        )
        vnet_result = poller.result()
        cprint(f"  Provisioned vnet {vnet_result.name} with prefix {vnet_result.address_space.address_prefixes}", "green")
    else:
        cprint(f"  Vnet {c_vnet_name} already exists", "yellow")


    ## Subnet
    print(f"Provisioning subnet {c_subnet_name} with prefix {MACHNET_ADDRESS_SPACE} in vnet {c_vnet_name}")
    try:
        existing_subnet = network_client.subnets.get(args.resource_group, c_vnet_name, c_subnet_name)
    except ResourceNotFoundError:
        print(f"  Subnet {c_subnet_name} does not exist in vnet {c_vnet_name}")
        existing_subnet = None

    if existing_subnet is None:
        poller = network_client.subnets.begin_create_or_update(
            args.resource_group,
            c_vnet_name,
            c_subnet_name,
            {"address_prefix": MACHNET_ADDRESS_SPACE},
        )
        subnet_result = poller.result()
        cprint(f"  Provisioned subnet {subnet_result.name} with prefix {subnet_result.address_prefix}", "green")
        subnet_id = subnet_result.id
    else:
        cprint(f"  Subnet {c_subnet_name} already exists in vnet {c_vnet_name}", "yellow")
        subnet_id = existing_subnet.id


    ## Network security group for the public NIC
    print(f"Provisioning network security group for the public NIC {c_public_nic_name} to allow SSH")
    poller = network_client.network_security_groups.begin_create_or_update(
        args.resource_group,
        c_public_nic_name,
        {
            "location": args.location,
            "security_rules": [
                {
                    "name": "ssh",
                    "protocol": "Tcp",
                    "source_port_range": "*",
                    "destination_port_range": "22",
                    "source_address_prefix": "*",
                    "destination_address_prefix": "*",
                    "access": "Allow",
                    "priority": 100,
                    "direction": "Inbound"
                }
            ],
        },
    )
    nsg_result = poller.result()
    cprint(f"  Provisioned network security group {nsg_result.name} with rules", "green")


    ## IP address for the public NIC
    print(f"Provisioning public IP address {c_ip_name}")
    poller = network_client.public_ip_addresses.begin_create_or_update(
        args.resource_group,
        c_ip_name,
        {
            "location": args.location,
            "sku": {"name": "Standard"},
            "public_ip_allocation_method": "Static",
            "public_ip_address_version": "IPV4",
        },
    )
    ip_address_result = poller.result()
    cprint(f"  Provisioned public IP address {ip_address_result.name} with address {ip_address_result.ip_address}", "green")


    ## Public NIC
    print(f"Provisioning network interface {c_public_nic_name} with IP {c_ip_name} and subnet {c_subnet_name}")
    poller = network_client.network_interfaces.begin_create_or_update(
        args.resource_group,
        c_public_nic_name,
        {
            "location": args.location,
            "ip_configurations": [
                {
                    "name": c_ip_config_name,
                    "subnet": {"id": subnet_id},
                    "public_ip_address": {"id": ip_address_result.id},
                }
            ],
            "network_security_group": {"id": nsg_result.id},
        },
    )
    public_nic_result = poller.result()
    cprint(f"  Provisioned public NIC {public_nic_result.name}", "green")


    ## Machnet NIC
    print(f"Provisioning network interface {c_machnet_nic_name} with subnet {c_subnet_name}")
    poller = network_client.network_interfaces.begin_create_or_update(
        args.resource_group,
        c_machnet_nic_name,
        {
            "location": args.location,
            "ip_configurations": [
                {
                    "name": c_ip_config_name,
                    "subnet": {"id": subnet_id},
                }
            ],
            "enable_accelerated_networking": True,
        },
    )
    machnet_nic_result = poller.result()
    cprint(f"  Provisioned Machnet NIC {machnet_nic_result.name}", "green")


    # Provision the VMs
    compute_client = ComputeManagementClient(credential, c_subscription_id)
    for vm_name_i in c_vm_names:
        username = os.environ.get("USER")
        password = ""
        print(f"Provisioning virtual machine {vm_name_i} with username {username} and password. ",
               "This operation might take a few minutes.")

        poller = compute_client.virtual_machines.begin_create_or_update(
            args.resource_group,
            vm_name_i,
            {
                "location": args.location,
                "storage_profile": {
                    "image_reference": {
                        "publisher": "Canonical",
                        "offer": "0001-com-ubuntu-server-jammy",
                        "sku": "22_04-lts-gen2",
                        "version": "latest",
                    }
                },
                "hardware_profile": {"vm_size": "Standard_F8s_v2"},
                "os_profile": {
                    "computer_name": vm_name_i,
                    "admin_username": username,
                    "admin_password": password,
                },
                "network_profile": {
                    "network_interfaces": [
                        { "id": public_nic_result.id, "primary": True },
                        { "id": machnet_nic_result.id, "primary": False }
                    ]
                },
            },
        )

        vm_result = poller.result()
        cprint(f"Provisioned virtual machine {vm_result.name}", "green")
        cprint(f"  Log-in using ssh {username}@{ip_address_result.ip_address}", "green")