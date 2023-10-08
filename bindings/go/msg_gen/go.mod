module msg_gen

go 1.20

require (
	github.com/buger/jsonparser v1.1.1
	github.com/golang/glog v1.1.0
	github.com/msr-machnet/machnet v0.0.0-00010101000000-000000000000
)

require github.com/HdrHistogram/hdrhistogram-go v1.1.2 // indirect

replace github.com/msr-machnet/machnet => ../machnet
