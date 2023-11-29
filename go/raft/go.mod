module machnet_raft

go 1.21

require (
	github.com/HdrHistogram/hdrhistogram-go v1.1.2
	github.com/buger/jsonparser v1.1.1
	github.com/golang/glog v1.2.0
	github.com/hashicorp/go-msgpack v0.5.5
	github.com/hashicorp/raft v1.6.0
	github.com/microsoft/machnet v1.0.0
)

replace github.com/microsoft/machnet => ../machnet

require (
	github.com/armon/go-metrics v0.4.1 // indirect
	github.com/fatih/color v1.16.0 // indirect
	github.com/hashicorp/go-hclog v1.5.0 // indirect
	github.com/hashicorp/go-immutable-radix v1.3.1 // indirect
	github.com/hashicorp/go-msgpack/v2 v2.1.1 // indirect
	github.com/hashicorp/golang-lru v1.0.2 // indirect
	github.com/mattn/go-colorable v0.1.13 // indirect
	github.com/mattn/go-isatty v0.0.20 // indirect
	golang.org/x/sys v0.15.0 // indirect
)
