#!/usr/bin/env python3
# Create AWS EC2 Instances along with VPC, Subnet, and Key Pair
# Usage: ./create_aws_ec2.py --aws_region us-west-2 --nickname tutorial --num_instances 2
#
# Requirements:
#  - pip3 install boto3 termcolor
#  - Configure AWS credentials using AWS CLI or environment variables

import argparse
import boto3
from boto3.resources.base import ServiceResource
from boto3.ec2 import Vpc, Subnet
from botocore.exceptions import ClientError
try:
    from termcolor import cprint
except ImportError:
    def cprint(*args, **kwargs):
        print(*args, **kwargs)

def create_vpc(ec2_resource: ServiceResource, vpc_name: str) -> Vpc:
    DEFAULT_VPC_CIDR_BLOCK: str = '10.0.0.0/16'
    try:
        vpc = ec2_resource.create_vpc(CidrBlock=DEFAULT_VPC_CIDR_BLOCK)
        vpc.create_tags(Tags=[{"Key": "Name", "Value": vpc_name}])
        vpc.wait_until_available()
        return vpc
    except ClientError as e:
        cprint(f"Error creating VPC: {e}", "red")
        raise

def create_subnet(ec2_resource: ServiceResource, vpc: Vpc, subnet_name: str) -> Subnet:
    DEFAULT_SUBNET_CIDR_BLOCK: str = '10.0.1.0/24'
    try:
        subnet = ec2_resource.create_subnet(VpcId=vpc.id, CidrBlock=DEFAULT_SUBNET_CIDR_BLOCK)
        subnet.create_tags(Tags=[{"Key": "Name", "Value": subnet_name}])
        return subnet
    except ClientError as e:
        cprint(f"Error creating Subnet: {e}", "red")
        raise

def create_key_pair(ec2_client, key_name: str) -> str:
    try:
        key_pair = ec2_client.create_key_pair(KeyName=key_name)
        with open(f"{key_name}.pem", "w") as file:
            file.write(key_pair['KeyMaterial'])
        return key_name
    except ClientError as e:
        if 'InvalidKeyPair.Duplicate' in str(e):
            cprint(f"KeyPair {key_name} already exists. Using existing KeyPair.", "yellow")
            return key_name
        else:
            cprint(f"Error creating KeyPair: {e}", "red")
            raise

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--aws_region", type=str, required=True, help="AWS region to create instances in")
    parser.add_argument("--nickname", type=str, required=True, help="Nickname to use as a prefix for resources")
    parser.add_argument("--num_instances", type=int, default=1, help="Number of EC2 instances to provision")
    args = parser.parse_args()

    if not args.nickname.isalnum():
        cprint("Error: nickname must be alphanumeric", "red")
        exit()

    ec2_resource = boto3.resource('ec2', region_name=args.aws_region)
    ec2_client = boto3.client('ec2', region_name=args.aws_region)

    c_vpc_name = f"{args.nickname}-{args.aws_region}-vpc"
    c_subnet_name = f"{args.nickname}-{args.aws_region}-subnet"

    vpc = create_vpc(ec2_resource, c_vpc_name)
    cprint(f"Created VPC: {vpc.id} with name tag {c_vpc_name}", "green")

    subnet = create_subnet(ec2_resource, vpc, c_subnet_name)
    cprint(f"Created subnet: {subnet.id} with name_tag", "green")

    exit(0)

    # Create Key Pair
    key_name = f"{args.nickname}_keypair"
    create_key_pair(ec2_client, key_name)
    cprint(f"Created and saved Key Pair: {key_name}.pem", "green")

    # Create EC2 instances
    try:
        instances = ec2_resource.create_instances(
            ImageId='ami-0abcdef1234567890',  # Replace with a valid AMI ID
            MinCount=1,
            MaxCount=args.num_instances,
            InstanceType='t2.micro',  # Replace with desired instance type
            KeyName=key_name,
            SubnetId=subnet.id
        )

        for instance in instances:
            cprint(f"Instance {instance.id} created", "green")
    except ClientError as e:
        cprint(f"Error creating instances: {e}", "red")

if __name__ == "__main__":
    main()
