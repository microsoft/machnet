#!/usr/bin/env python3
# Create AWS EC2 Instances for testing with Machnet
# Usage: ./create_aws_ec2.py --aws_region us-east-1 --nickname tutorial --num_instances 1 --key_name <AWS SSH key_name>
#
# Requirements:
#  - pip3 install boto3 termcolor
#  - Configure AWS credentials using AWS CLI or environment variables
#
# Formatter: black

import argparse
import boto3
from boto3.resources.base import ServiceResource
from botocore.exceptions import ClientError

try:
    from termcolor import cprint
except ImportError:

    def cprint(*args, **kwargs):
        print(*args, **kwargs)


def create_vpc_helper(ec2_resource: ServiceResource, vpc_name: str) -> dict:
    for vpc in ec2_resource.vpcs.all():
        if vpc.tags:
            for tag in vpc.tags:
                if tag["Key"] == "Name" and tag["Value"] == vpc_name:
                    cprint(
                        f"VPC {vpc.id} with name {vpc_name} already exists. Using existing VPC.",
                        "yellow",
                    )
                    return vpc

    DEFAULT_VPC_CIDR_BLOCK: str = "10.0.0.0/16"
    try:
        vpc = ec2_resource.create_vpc(CidrBlock=DEFAULT_VPC_CIDR_BLOCK)
        vpc.create_tags(Tags=[{"Key": "Name", "Value": vpc_name}])
        vpc.wait_until_available()
        cprint(f"Created VPC: {vpc.id} with name {vpc_name}", "green")
        return vpc
    except ClientError as e:
        cprint(f"Error creating VPC: {e}", "red")
        exit(-1)


def create_subnet(ec2_resource: ServiceResource, vpc: dict, subnet_name: str) -> dict:
    for subnet in vpc.subnets.all():
        if subnet.tags:
            for tag in subnet.tags:
                if tag["Key"] == "Name" and tag["Value"] == subnet_name:
                    cprint(
                        f"Subnet {subnet.id} with name {subnet_name} already exists. Using existing subnet.",
                        "yellow",
                    )
                    return subnet

    DEFAULT_SUBNET_CIDR_BLOCK: str = "10.0.1.0/24"
    try:
        subnet = ec2_resource.create_subnet(
            VpcId=vpc.id, CidrBlock=DEFAULT_SUBNET_CIDR_BLOCK
        )
        subnet.create_tags(Tags=[{"Key": "Name", "Value": subnet_name}])
        cprint(f"Created subnet: {subnet.id} with name {subnet_name}", "green")
        return subnet
    except ClientError as e:
        cprint(f"Error creating subnet: {e}", "red")
        exit(-1)


def create_key_pair(ec2_client, key_name: str) -> str:
    try:
        key_pair = ec2_client.create_key_pair(KeyName=key_name)
        with open(f"{key_name}.pem", "w") as file:
            file.write(key_pair["KeyMaterial"])
        return key_name
    except ClientError as e:
        if "InvalidKeyPair.Duplicate" in str(e):
            cprint(
                f"KeyPair {key_name} already exists. Using existing KeyPair.", "yellow"
            )
            return key_name
        else:
            cprint(f"Error creating KeyPair: {e}", "red")
            raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--aws_region",
        type=str,
        required=True,
        help="AWS region to create instances in",
        choices=["us-east-1", "eu-central-1"],
    )
    parser.add_argument(
        "--nickname",
        type=str,
        required=True,
        help="Nickname to use as a prefix for resources",
    )
    parser.add_argument(
        "--key_name",
        type=str,
        required=True,
        help="Name of an existing AWS SSH key pair",
    )
    parser.add_argument(
        "--num_instances",
        type=int,
        default=1,
        help="Number of EC2 instances to provision",
    )
    args = parser.parse_args()

    if not args.nickname.isalnum():
        cprint("Error: nickname must be alphanumeric", "red")
        exit()

    c_vpc_name = f"{args.nickname}-{args.aws_region}-vpc"
    c_subnet_name = f"{args.nickname}-{args.aws_region}-subnet"
    c_sg_name = f"{args.nickname}-{args.aws_region}-sg"
    c_base_instance_name = f"{args.nickname}-{args.aws_region}-instance"
    c_base_machnet_nic_name = f"{args.nickname}-{args.aws_region}-machnet-nic"

    ec2: ServiceResource = boto3.resource("ec2", region_name=args.aws_region)
    vpc = create_vpc_helper(ec2, c_vpc_name)
    subnet = create_subnet(ec2, vpc, c_subnet_name)

    security_group_exists = False
    sg = None
    for sg_i in vpc.security_groups.filter(
        Filters=[{"Name": "group-name", "Values": [c_sg_name]}]
    ):
        if sg_i.group_name == c_sg_name:
            security_group_exists = True
            sg = sg_i
            cprint(f"Security group {c_sg_name} {sg_i.id} exists.", "yellow")
            break

    if not security_group_exists:
        sg = ec2.create_security_group(
            GroupName=c_sg_name, Description="Machnet security group", VpcId=vpc.id
        )
        cprint(f"Created security group {c_sg_name} = {sg.id}", "green")

    try:
        sg.authorize_ingress(
            IpPermissions=[
                {
                    "FromPort": 22,
                    "ToPort": 22,
                    "IpProtocol": "tcp",
                    "IpRanges": [
                        {"CidrIp": "0.0.0.0/0", "Description": "internet"},
                    ],
                }
            ],
        )
        cprint(f"Added SSH access ability to security group {c_sg_name}", "green")
    except ClientError as e:
        if "InvalidPermission.Duplicate" in str(e):
            cprint(f"SSH already on for security group {c_sg_name}", "yellow")

    # Add more from https://cloud-images.ubuntu.com/locator/ec2/
    if args.aws_region == "us-east-1":
        ami_id = "ami-0c7217cdde317cfec"
    elif args.aws_region == "eu-central-1":
        ami_id = "ami-0fc02b454efabb390"

    instance = None
    for i in range(args.num_instances):
        instance_name_i = f"{c_base_instance_name}-{i}"

        # Check if instance already exists
        instance_already_exists = False
        for instance_i in ec2.instances.all():
            if instance_i.tags:
                for tag in instance_i.tags:
                    if tag["Key"] == "Name" and tag["Value"] == instance_name_i:
                        cprint(
                            f"Instance {instance_name_i} {instance_i.id} already exists.",
                            "yellow",
                        )
                        instance_already_exists = True
                        instance = instance_i

        if not instance_already_exists:
            print(f"Creating new instance with name {instance_name_i}")

            try:
                instances = ec2.create_instances(
                    ImageId=ami_id,
                    MinCount=1,
                    MaxCount=1,
                    InstanceType="t2.micro",
                    KeyName=args.key_name,
                    NetworkInterfaces=[
                        {
                            "DeviceIndex": 0,
                            "SubnetId": subnet.id,
                            "Groups": [sg.id],
                            "AssociatePublicIpAddress": True,
                        },
                    ],
                    TagSpecifications=[
                        {
                            "ResourceType": "instance",
                            "Tags": [
                                {"Key": "Name", "Value": instance_name_i},
                            ],
                        },
                    ],
                )
                instance = instances[0]
                cprint(f"Instance {instance_name_i} {instance.id} created", "green")
            except ClientError as e:
                cprint(f"Error creating instances: {e}", "red")

        # Check if machnet_nic already exists
        machnet_nic_already_exists = False
        machnet_nic = None
        machnet_nic_name_i = f"{c_base_machnet_nic_name}-{i}"
        for nic in ec2.network_interfaces.all():
            if nic.tag_set:
                for tag in nic.tag_set:
                    if tag["Key"] == "Name" and tag["Value"] == machnet_nic_name_i:
                        cprint(
                            f"NIC {machnet_nic_name_i} {nic.id} already exists.",
                            "yellow",
                        )
                        machnet_nic_already_exists = True
                        machnet_nic = nic

        if not machnet_nic_already_exists:
            machnet_nic = ec2.create_network_interface(
                SubnetId=subnet.id,
                Description="Machnet network interface",
                TagSpecifications=[
                    {
                        "ResourceType": "network-interface",
                        "Tags": [
                            {"Key": "Name", "Value": f"{c_base_machnet_nic_name}-{i}"},
                        ],
                    },
                ],
            )
            cprint(f"Created NIC {machnet_nic_name_i} {machnet_nic.id}", "green")

        # Wait till the instance is running
        print(f"Waiting for instance {instance_name_i} to be running")
        instance.wait_until_running()

        try:
            machnet_nic.attach(InstanceId=instance.id, DeviceIndex=1)
            cprint(f"Attached NIC {machnet_nic_name_i} to {instance_name_i}", "green")
        except ClientError as e:
            if "already has an interface attached at device index" in str(e):
                cprint(
                    f"NIC {machnet_nic_name_i} already attached to {instance_name_i}",
                    "yellow",
                )
            else:
                cprint(f"Error attaching NIC: {e}", "red")


if __name__ == "__main__":
    main()
