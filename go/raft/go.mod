module machnet_raft

go 1.21

toolchain go1.21.3

require (
	github.com/HdrHistogram/hdrhistogram-go v1.1.2
	github.com/buger/jsonparser v1.1.1
	github.com/golang/glog v1.1.2
	github.com/hashicorp/go-msgpack v0.5.5
	github.com/hashicorp/raft v1.5.0
	github.com/microsoft/machnet v1.0.0
	github.com/tjarratt/babble v0.0.0-20210505082055-cbca2a4833c1
)

replace github.com/microsoft/machnet => ../machnet

require (
	github.com/armon/go-metrics v0.4.1 // indirect
	github.com/fatih/color v1.13.0 // indirect
	github.com/google/go-cmp v0.5.9 // indirect
	github.com/hashicorp/go-hclog v1.5.0 // indirect
	github.com/hashicorp/go-immutable-radix v1.0.0 // indirect
	github.com/hashicorp/go-msgpack/v2 v2.1.1 // indirect
	github.com/hashicorp/golang-lru v0.5.0 // indirect
	github.com/mattn/go-colorable v0.1.12 // indirect
	github.com/mattn/go-isatty v0.0.14 // indirect
	github.com/onsi/ginkgo v1.16.5 // indirect
	github.com/onsi/gomega v1.27.8 // indirect
	golang.org/x/sys v0.13.0 // indirect
)
