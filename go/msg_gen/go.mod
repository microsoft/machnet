module msg_gen

go 1.21

require (
	github.com/buger/jsonparser v1.1.1
	github.com/golang/glog v1.1.2
	github.com/microsoft/machnet v1.0.0
)

require github.com/HdrHistogram/hdrhistogram-go v1.1.2
replace github.com/microsoft/machnet => ../machnet
