# Machnet Rust Bindings

[![Crates.io](https://img.shields.io/crates/v/machnet.svg)](https://crates.io/crates/machnet)
[![Docs.rs](https://docs.rs/machnet/badge.svg)](https://docs.rs/machnet)
[![Build](https://github.com/microsoft/machnet/actions/workflows/build.yml/badge.svg?event=push)](https://github.com/microsoft/machnet)
![GitHub License](https://img.shields.io/github/license/microsoft/machnet)

This repository contains the Rust FFI bindings for **Machnet**.

Machnet provides an easy way for applications to reduce their datacenter networking latency via kernel-bypass (DPDK-based) messaging.
Distributed applications like databases and finance can use Machnet as the networking library to get sub-100 microsecond tail latency at high message rates, e.g., 750,000 1KB request-reply messages per second on Azure F8s_v2 VMs with 61 microsecond P99.9 round-trip latency.

We support a variety of cloud (Azure, AWS, GCP) and bare-metal platforms, OSs, and NICs, evaluated in [PERFORMANCE_REPORT.md](../../docs/PERFORMANCE_REPORT.md).

## Getting Started

To use the Machnet Rust bindings, add the following to your `Cargo.toml`:

```toml
[dependencies]
machnet = "0.1.7"
```

## Open Source Project

This project is an open-source initiative under Microsoft. We welcome contributions and suggestions from the community!
See [CONTRIBUTING.md](../../CONTRIBUTING.md) for more details.

<img src="https://evergreenleadership.com/wp-content/uploads/2019/05/microsoft-logo-png-transparent-20.png" alt="Microsoft" width="100"/>
