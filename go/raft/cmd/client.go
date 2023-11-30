package main

import (
	"bytes"
	"encoding/gob"
	"flag"
	"fmt"
	"math"
	"math/rand"
	"os"
	"runtime/pprof"
	"time"

	"github.com/HdrHistogram/hdrhistogram-go"
	"github.com/buger/jsonparser"
	"github.com/golang/glog"
	"github.com/microsoft/machnet"
)

var (
	localHostname  = flag.String("local_hostname", "", "Local hostname in the hosts JSON file.")
	remoteHostname = flag.String("remote_hostname", "", "Local hostname in the hosts JSON file.")
	appPort        = flag.Int("app_port", 888, "Port to listen on for application traffic.")
	configJson     = flag.String("config_json", "../servers.json", "Path to the JSON file containing the hosts config.")
	cpuProfile     = flag.String("cpuProfile", "", "write cpu profile to file")
	keySize        = flag.Int("key_size", 16, "Key size")
	valueSize      = flag.Int("value_size", 64, "Value size")
)

type Payload struct {
	Key   string
	Value string
}

func main() {
	flag.Parse()
	if *cpuProfile != "" {
		f, err := os.Create(*cpuProfile)
		if err != nil {
			glog.Fatal(err)
		}
		err = pprof.StartCPUProfile(f)
		if err != nil {
			glog.Errorf("Main: failed to create cpu profile: %+v", err)
		}
		defer pprof.StopCPUProfile()
	}

	jsonBytes, err := os.ReadFile(*configJson)
	if err != nil {
		glog.Fatalf("Main: failed to read config file: %v", err)
	}

	localIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", *localHostname, "ipv4_addr")
	remoteIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", *remoteHostname, "ipv4_addr")

	glog.Info("MAin: trying to connect to ", remoteIp, ":", *appPort, " from ", localIp)

	ret := machnet.Init()
	if ret != 0 {
		glog.Fatal("Main: failed to initialize the Machnet library.")
	}

	var channelCtx *machnet.MachnetChannelCtx = machnet.Attach() // TODO: Defer machnet.Detach()?

	if channelCtx == nil {
		glog.Fatal("Main: failed to attach to the channel.")
	}

	var flow machnet.MachnetFlow
	ret, flow = machnet.Connect(channelCtx, localIp, remoteIp, uint(*appPort))
	if ret != 0 {
		glog.Fatal("Main: failed to connect to remote host.")
	}
	glog.Info("Main: connected ", localIp, " <-> ", remoteIp, ":", *appPort, "]")

	histogram := hdrhistogram.New(1, 1000000, 3)
	lastRecordedTime := time.Now()

	for {

		key := generateRandomKey(*keySize)
		value := generateRandomValue(*valueSize)
		payload := Payload{
			key, value,
		}

		var buf bytes.Buffer
		enc := gob.NewEncoder(&buf)

		if err := enc.Encode(payload); err != nil {
			glog.Errorf("Main: failed to encode payload: %+v", err)
			continue
		}

		payloadBytes := buf.Bytes()
		payloadLen := uint(len(payloadBytes))

		start := time.Now()

		ret := machnet.SendMsg(channelCtx, flow, &payloadBytes[0], payloadLen)
		if ret != 0 {
			glog.Error("Main: failed to send payload")
			continue
		}

		responseBuff := make([]byte, 64)
		recvBytes, _ := machnet.Recv(channelCtx, &responseBuff[0], 64)
		for recvBytes == 0 {
			recvBytes, _ = machnet.Recv(channelCtx, &responseBuff[0], 64)
		}

		if err := histogram.RecordValue(time.Since(start).Microseconds()); err != nil {
			glog.Errorf("Main: failed to record value to histogram: %v", err)
		}

		if time.Since(lastRecordedTime) > 1*time.Second {
			percentileValues := histogram.ValueAtPercentiles([]float64{50.0, 95.0, 99.0, 99.9})
			glog.Infof("[RTT: 50p %.3f us, 95p %.3f us, 99p %.3f us, 99.9p %.3f us,  RPS: %d]",
				float64(percentileValues[50.0]), float64(percentileValues[95.0]),
				float64(percentileValues[99.0]), float64(percentileValues[99.9]),
				histogram.TotalCount())
			histogram.Reset()
			lastRecordedTime = time.Now()
		}
		time.Sleep(1 * time.Second)
	}
}

func generateRandomKey(length int) string {
	source := rand.NewSource(time.Now().UnixNano())
	randRange := rand.New(source)
	max_ := int(math.Pow10(length)) - 1
	number := randRange.Intn(max_)
	return fmt.Sprintf("%0*d", length, number)
}

func generateRandomValue(length int) string {
	const charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
	source := rand.NewSource(time.Now().UnixNano())
	randRange := rand.New(source)
	b := make([]byte, length)
	for i := range b {
		b[i] = charset[randRange.Intn(len(charset))]
	}
	return string(b)
}
