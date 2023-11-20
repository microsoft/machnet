package main

import (
	"flag"
	"math"
	"os"
	"sync"
	"time"

	"github.com/HdrHistogram/hdrhistogram-go"
	"github.com/buger/jsonparser"
	"github.com/golang/glog"
	"github.com/microsoft/machnet"
)

// TODO: Check Path here.
var configJson = "../../servers.json"

var localHostname = ""
var remoteHostname = ""
var remotePort uint = 888
var msgSize = 64
var msgWindow uint64 = 8
var msgNr uint64 = math.MaxUint64
var activeGenerator = false
var verify = false
var latency = false

type stats struct {
	txSuccess  uint64
	txBytes    uint64
	rxCount    uint64
	rxBytes    uint64
	errTxDrops uint64
	snapshot   *hdrhistogram.Snapshot
}

type taskCtx struct {
	ctx     *machnet.MachnetChannelCtx
	flow    machnet.MachnetFlow
	msgData []uint8
	rxMsg   []uint8
}

// TODO: Currently, allows for exactly `msg_size` bytes of data.
func newTaskCtx(ctx *machnet.MachnetChannelCtx, flow machnet.MachnetFlow) *taskCtx {
	var task = new(taskCtx)

	task.ctx = ctx
	task.flow = flow
	task.msgData = make([]uint8, msgSize)
	task.rxMsg = make([]uint8, msgSize)

	// Initialize the message data.
	task.msgData[0] = 1
	for i := 1; i < msgSize; i++ {
		task.msgData[i] = 0
	}

	return task
}

func newStats() *stats {
	var stats = new(stats)
	stats.txSuccess = 0
	stats.txBytes = 0
	stats.rxCount = 0
	stats.rxBytes = 0
	stats.errTxDrops = 0

	// Create a dummy snapshot. Used only at serialization.
	tempHistogram := hdrhistogram.New(1, 1000000, 3)
	stats.snapshot = tempHistogram.Export()
	return stats
}

func initFlags() {
	flag.StringVar(&localHostname, "local_hostname", localHostname, "Local hostname in the hosts JSON file.")
	flag.StringVar(&remoteHostname, "remote_hostname", remoteHostname, "Remote hostname in the hosts JSON file.")
	flag.UintVar(&remotePort, "remote_port", remotePort, "Remote port to connect to.")
	flag.IntVar(&msgSize, "msg_size", msgSize, "TX Message size in bytes.")
	flag.Uint64Var(&msgWindow, "msg_window", msgWindow, "Maximum number of messages in flight.")
	flag.Uint64Var(&msgNr, "msg_nr", msgNr, "Number of messages to send.")
	flag.BoolVar(&activeGenerator, "active_generator", activeGenerator, "When 'true' this host is generating the traffic, otherwise it is bouncing.")
	flag.BoolVar(&verify, "verify", verify, "When 'true' verify the payload of received messages.")
	flag.BoolVar(&latency, "latency", latency, "When 'true' measure the latency of the messages.")
	flag.Parse()
}

// Function to report the stats.
func (stats *stats) reportStats() {
	txThroughput := float64(stats.txBytes) * 8 / (1000 * 1000 * 1000)
	rxThroughput := float64(stats.rxBytes) * 8 / (1000 * 1000 * 1000)

	if latency {
		histogram := hdrhistogram.Import(stats.snapshot)
		percentileValues := histogram.ValueAtPercentiles([]float64{50.0, 95.0, 99.0, 99.9})
		glog.Infof("[RTT: 50p %.3f us, 95p %.3f us, 99p %.3f us, 99.9p %.3f us, %d] [TX_DROP MPS: %d]",
			float64(percentileValues[50.0])/1000, float64(percentileValues[95.0])/1000,
			float64(percentileValues[99.0])/1000, float64(percentileValues[99.9])/1000,
			histogram.TotalCount(), stats.errTxDrops)

	} else {
		glog.Infof("[TX MPS: %d (%f Gbps)] [RX MPS: %d (%f Gbps)] [TX_DROP MPS: %d]",
			stats.txSuccess, txThroughput, stats.rxCount, rxThroughput,
			stats.errTxDrops)
	}
}

// Function to accumulate the stats.
func (stats *stats) accumulateStats(other *stats) {
	stats.txSuccess += other.txSuccess
	stats.txBytes += other.txBytes
	stats.rxCount += other.rxCount
	stats.rxBytes += other.rxBytes
	stats.errTxDrops += other.errTxDrops

	otherHistogram := hdrhistogram.Import(other.snapshot)
	statsHistogram := hdrhistogram.Import(stats.snapshot)
	statsHistogram.Merge(otherHistogram)
	stats.snapshot = statsHistogram.Export()
}

// Function to return the difference between two stats.
func (current *stats) diffStats(other *stats) stats {
	var diff stats
	diff.txSuccess = current.txSuccess - other.txSuccess
	diff.txBytes = current.txBytes - other.txBytes
	diff.rxCount = current.rxCount - other.rxCount
	diff.rxBytes = current.rxBytes - other.rxBytes
	diff.errTxDrops = current.errTxDrops - other.errTxDrops

	// Histogram does not depend on the previous stats.
	diff.snapshot = current.snapshot
	return diff
}

func tx(task *taskCtx, stats *stats) {
	channelCtx := task.ctx

	// Return if we have already sent the required number of messages.
	if msgNr <= stats.txSuccess {
		return
	}

	// Don't send more than msg_window messages at a time.
	if stats.txSuccess-stats.rxCount >= msgWindow {
		return
	}

	ret := machnet.SendMsg(channelCtx, task.flow, &task.msgData[0], uint(msgSize))
	if ret == 0 {
		stats.txSuccess += 1
		stats.txBytes += uint64(msgSize)
	} else {
		stats.errTxDrops += 1
	}
}

func rx(task *taskCtx, stats *stats) {
	channelCtx := task.ctx

	err, _ := machnet.RecvMsg(channelCtx, &task.rxMsg[0], uint(msgSize))
	if err != 0 {
		return
	}

	// Verify if the received message is the same as the sent message.
	if verify {
		for i := 0; i < msgSize; i++ {
			if task.rxMsg[i] != task.msgData[i] {
				glog.Fatalf("Received message does not match the sent message: %d %d %d", i, task.rxMsg[i], task.msgData[i])
			}
		}
	}

	stats.rxCount += 1
	stats.rxBytes += uint64(msgSize)
}

// Ping a message and wait for a response for latency measurements.
func ping(task *taskCtx, stats *stats, histogram *hdrhistogram.Histogram) {
	channelCtx := task.ctx

	// Return if we have already sent the required number of messages.
	if msgNr <= stats.txSuccess {
		return
	}

	// Start timer.
	start := time.Now()

	ret := machnet.SendMsg(channelCtx, task.flow, &task.msgData[0], uint(msgSize))
	if ret == 0 {
		stats.txSuccess += 1
		stats.txBytes += uint64(msgSize)
	} else {
		stats.errTxDrops += 1
	}

	// Keep reading until we get a message from the same flow.
	err, _ := machnet.RecvMsg(channelCtx, &task.rxMsg[0], uint(msgSize))
	for err != 0 {
		err, _ = machnet.RecvMsg(channelCtx, &task.rxMsg[0], uint(msgSize))
	}

	// Stop timer.
	elapsed := time.Since(start)
	histogram.RecordValue(elapsed.Nanoseconds())

	// Verify if the received message is the same as the sent message.
	if verify {
		for i := 0; i < msgSize; i++ {
			if task.rxMsg[i] != task.msgData[i] {
				glog.Fatalf("Received message does not match the sent message: %d %d %d", i, task.rxMsg[i], task.msgData[i])
			}
		}
	}

	stats.rxCount += 1
	stats.rxBytes += uint64(msgSize)
}

func bounce(task *taskCtx, stats *stats) {
	channelCtx := task.ctx

	err, flowInfo := machnet.RecvMsg(channelCtx, &task.rxMsg[0], uint(msgSize))
	if err != 0 {
		return
	}
	stats.rxCount += 1
	stats.rxBytes += uint64(msgSize)

	// Swap the source and destination IP addresses.
	tmpFlow := flowInfo
	flowInfo.SrcIp = tmpFlow.DstIp
	flowInfo.DstIp = tmpFlow.SrcIp
	flowInfo.SrcPort = tmpFlow.DstPort
	flowInfo.DstPort = tmpFlow.SrcPort

	// Prepare to send back the message.
	ret := machnet.SendMsg(channelCtx, flowInfo, &task.rxMsg[0], uint(msgSize))
	if ret == 0 {
		stats.txSuccess += 1
		stats.txBytes += uint64(msgSize)
	} else {
		stats.errTxDrops += 1
	}
}

func runWorker(task *taskCtx, activeGenerator bool, tick *time.Ticker, statsChan chan<- stats) {
	taskStats := newStats()
	countHistogram := hdrhistogram.New(1, 1000000, 3)
	if activeGenerator {
		for {
			if latency {
				ping(task, taskStats, countHistogram)
			} else {
				tx(task, taskStats)
				rx(task, taskStats)
			}

			// Report the stats every second.
			select {
			case <-tick.C:
				if latency {
					taskStats.snapshot = countHistogram.Export()
					countHistogram.Reset()
				}
				statsChan <- *taskStats
			default:
				continue
			}
		}
	} else {
		for {
			bounce(task, taskStats)

			// Report the stats every second.
			select {
			case <-tick.C:
				statsChan <- *taskStats
			default:
				continue
			}
		}
	}
}

func main() {
	initFlags()

	if activeGenerator {
		glog.Info("Starting in active generator mode.")
	} else {
		glog.Info("Starting in passive message bouncing mode.")
	}

	ret := machnet.Init()
	if ret != 0 {
		glog.Fatal("Failed to initialize the Machnet library.")
	}

	var channelCtx *machnet.MachnetChannelCtx = machnet.Attach() // TODO: Defer machnet.Detach()?

	if channelCtx == nil {
		glog.Fatal("Failed to attach to the channel.")
	}

	// Read the contents of file config_json into a byte array.
	jsonBytes, err := os.ReadFile(configJson)
	if err != nil {
		glog.Fatal("Failed to read config file.")
	}

	// Parse the json file to get the local_ip.
	localIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", localHostname, "ipv4_addr")

	var flow machnet.MachnetFlow
	if activeGenerator {
		// Parse the json file to get the remote_ip.
		remoteIp, _ := jsonparser.GetString(jsonBytes, "hosts_config", remoteHostname, "ipv4_addr")

		// Initiate connection to the remote host.
		ret, flow = machnet.Connect(channelCtx, localIp, remoteIp, remotePort)
		if ret != 0 {
			glog.Fatal("Failed to connect to remote host.")
		}
		glog.Info("[CONNECTED] [", localIp, " <-> ", remoteIp, ":", remotePort, "]")
	} else {
		// Listen for incoming connections.
		ret = machnet.Listen(channelCtx, localIp, remotePort)
		if ret != 0 {
			glog.Fatal("Failed to listen for incoming connections.")
		}
		glog.Info("[LISTENING] [", localIp, ":", remotePort, "]")
	}

	// Create a task context for all CPU cores.
	taskCtx := newTaskCtx(channelCtx, flow)

	// Number of CPU cores to run on.
	cpuCores := 1

	// Create a channel to receive the stats.
	statsChan := make(chan stats, cpuCores)
	var wg sync.WaitGroup
	wg.Add(cpuCores)

	for w := 1; w <= cpuCores; w++ {
		// Add ticker to print stats.
		ticker := time.NewTicker(1 * time.Second)
		defer ticker.Stop()
		go runWorker(taskCtx, activeGenerator, ticker, statsChan)
	}

	// Print the stats.
	go func() {
		accumulatedStats := newStats() // Accumulate the stats from all CPU cores.
		var prevStats stats            // Hold the previously accumulated stats.
		var reportStats stats          // Hold the difference between the current and previous stats.
		for {
			accumulatedStats = newStats()
			for i := 0; i < cpuCores; i++ {
				s := <-statsChan
				accumulatedStats.accumulateStats(&s)
			}
			reportStats = accumulatedStats.diffStats(&prevStats)
			reportStats.reportStats()
			prevStats = *accumulatedStats
		}
	}()

	wg.Wait()
}
