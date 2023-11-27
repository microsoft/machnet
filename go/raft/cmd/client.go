package main

import (
	"flag"
	"os"
	"runtime/pprof"
	"time"

	"github.com/HdrHistogram/hdrhistogram-go"
	"github.com/buger/jsonparser"
	"github.com/golang/glog"
	"github.com/microsoft/machnet"
	"github.com/tjarratt/babble"
)

var (
	localHostname  = flag.String("local_hostname", "", "Local hostname in the hosts JSON file.")
	remoteHostname = flag.String("remote_hostname", "", "Local hostname in the hosts JSON file.")
	appPort        = flag.Int("app_port", 888, "Port to listen on for application traffic.")
	configJson     = flag.String("config_json", "../servers.json", "Path to the JSON file containing the hosts config.")
	cpuprofile     = flag.String("cpuprofile", "", "write cpu profile to file")
)

func main() {
	flag.Parse()
	if *cpuprofile != "" {
		f, err := os.Create(*cpuprofile)
		if err != nil {
			glog.Fatal(err)
		}
		pprof.StartCPUProfile(f)
		defer pprof.StopCPUProfile()
	}

	// Start NSaaS channel.
	ret := machnet.Init()
	if ret != 0 {
		glog.Fatal("Failed to initialize the nsaas library.")
	}

	var channelCtx *machnet.MachnetChannelCtx = machnet.Attach() // TODO: Defer machnet.Detach()?

	if channelCtx == nil {
		glog.Fatal("Failed to attach to the channel.")
	}

	// Read the contents of file config_json into a byte array.
	jsonBytes, err := os.ReadFile(*configJson)
	if err != nil {
		glog.Fatal("Failed to read config file.")
	}

	// Parse the json file to get the local ip and remote ip.
	localIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", *localHostname, "ipv4_addr")
	remoteIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", *remoteHostname, "ipv4_addr")
	glog.Info("Trying to connect to ", remoteIp, ":", *appPort, " from ", localIp)

	// Initiate connection to the remote host.
	var flow machnet.MachnetFlow
	ret, flow = machnet.Connect(channelCtx, localIp, remoteIp, uint(*appPort))
	if ret != 0 {
		glog.Fatal("Failed to connect to remote host.")
	}
	glog.Info("[CONNECTED] [", localIp, " <-> ", remoteIp, ":", *appPort, "]")

	// Generate random words and send them to the remote host.
	babbler := babble.NewBabbler()
	babbler.Count = 1

	count := 0
	histogram := hdrhistogram.New(1, 100000000, 3)
	lastRecordedTime := time.Now()
	for {
		word := babbler.Babble()
		wordBytes := []byte(word)
		wordLen := len(wordBytes)
		if wordLen == 0 {
			continue
		}

		// Time the SendMsg() call.
		start := time.Now()

		ret := machnet.SendMsg(channelCtx, flow, &wordBytes[0], uint(wordLen))
		if ret != 0 {
			glog.Fatal("Failed to send word.")
		}

		// Receive the response from the remote host on the flow.
		responseBuff := make([]byte, 4)

		// Keep reading until we get a message from the same flow.
		recvBytes, _ := machnet.Recv(channelCtx, &responseBuff[0], 4)
		for recvBytes == 0 {
			recvBytes, _ = machnet.Recv(channelCtx, &responseBuff[0], 4)
		}

		elapsed := time.Since(start)
		count += 1
		// glog.Info("Added word: ", word, " [", elapsed.Microseconds(), " us, ", elapsed.Nanoseconds(), " ns]")
		histogram.RecordValue(elapsed.Nanoseconds())

		if time.Since(lastRecordedTime) > 1*time.Second {
			percentileValues := histogram.ValueAtPercentiles([]float64{50.0, 95.0, 99.0, 99.9})
			glog.Infof("[RTT: 50p %.3f us, 95p %.3f us, 99p %.3f us, 99.9p %.3f us, Words added: %d]",
				float64(percentileValues[50.0])/1000, float64(percentileValues[95.0])/1000,
				float64(percentileValues[99.0])/1000, float64(percentileValues[99.9])/1000,
				count)
			count = 0
			histogram.Reset()
			lastRecordedTime = time.Now()
		}

		// Sleep for 1ms.
		// time.Sleep(1 * time.Millisecond)
	}
}
