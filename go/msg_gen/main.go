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
	machnet "github.com/msr-machnet/machnet"
)

// TODO: Check Path here.
var config_json string = "../../servers.json"

var local_hostname string = ""
var remote_hostname string = ""
var remote_port uint = 888
var msg_size int = 64
var msg_window uint64 = 8
var msg_nr uint64 = math.MaxUint64
var active_generator bool = false
var verify bool = false
var latency bool = false

type stats struct {
	tx_success   uint64
	tx_bytes     uint64
	rx_count     uint64
	rx_bytes     uint64
	err_tx_drops uint64
	snapshot     *hdrhistogram.Snapshot
}

type task_ctx struct {
	ctx      *machnet.MachnetChannelCtx
	flow     machnet.MachnetFlow
	msg_data []uint8
	rx_msg   []uint8
}

// TODO: Currently, allows for exactly `msg_size` bytes of data.
func new_task_ctx(ctx *machnet.MachnetChannelCtx, flow machnet.MachnetFlow) *task_ctx {
	var task *task_ctx = new(task_ctx)

	task.ctx = ctx
	task.flow = flow
	task.msg_data = make([]uint8, msg_size)
	task.rx_msg = make([]uint8, msg_size)

	// Initialize the message data.
	task.msg_data[0] = 1
	for i := 1; i < msg_size; i++ {
		task.msg_data[i] = 0
	}

	return task
}

func new_stats() *stats {
	var stats *stats = new(stats)
	stats.tx_success = 0
	stats.tx_bytes = 0
	stats.rx_count = 0
	stats.rx_bytes = 0
	stats.err_tx_drops = 0

	// Create a dummy snapshot. Used only at serialization.
	temp_histogram := hdrhistogram.New(1, 1000000, 3)
	stats.snapshot = temp_histogram.Export()
	return stats
}

func init_flags() {
	flag.StringVar(&local_hostname, "local_hostname", local_hostname, "Local hostname in the hosts JSON file.")
	flag.StringVar(&remote_hostname, "remote_hostname", remote_hostname, "Remote hostname in the hosts JSON file.")
	flag.UintVar(&remote_port, "remote_port", uint(remote_port), "Remote port to connect to.")
	flag.IntVar(&msg_size, "msg_size", msg_size, "TX Message size in bytes.")
	flag.Uint64Var(&msg_window, "msg_window", msg_window, "Maximum number of messages in flight.")
	flag.Uint64Var(&msg_nr, "msg_nr", msg_nr, "Number of messages to send.")
	flag.BoolVar(&active_generator, "active_generator", active_generator, "When 'true' this host is generating the traffic, otherwise it is bouncing.")
	flag.BoolVar(&verify, "verify", verify, "When 'true' verify the payload of received messages.")
	flag.BoolVar(&latency, "latency", latency, "When 'true' measure the latency of the messages.")
	flag.Parse()
}

// Function to report the stats.
func (stats *stats) report_stats() {
	tx_throughput := float64(stats.tx_bytes) * 8 / (1000 * 1000 * 1000)
	rx_throughput := float64(stats.rx_bytes) * 8 / (1000 * 1000 * 1000)

	if latency {
		histogram := hdrhistogram.Import(stats.snapshot)
		perc_values := histogram.ValueAtPercentiles([]float64{50.0, 95.0, 99.0, 99.9})
		glog.Infof("[RTT: 50p %.3f us, 95p %.3f us, 99p %.3f us, 99.9p %.3f us, %d] [TX_DROP MPS: %d]",
			float64(perc_values[50.0])/1000, float64(perc_values[95.0])/1000,
			float64(perc_values[99.0])/1000, float64(perc_values[99.9])/1000,
			histogram.TotalCount(), stats.err_tx_drops)

	} else {
		glog.Infof("[TX MPS: %d (%f Gbps)] [RX MPS: %d (%f Gbps)] [TX_DROP MPS: %d]",
			stats.tx_success, tx_throughput, stats.rx_count, rx_throughput,
			stats.err_tx_drops)
	}
}

// Function to accumulate the stats.
func (stats *stats) accumulate_stats(other *stats) {
	stats.tx_success += other.tx_success
	stats.tx_bytes += other.tx_bytes
	stats.rx_count += other.rx_count
	stats.rx_bytes += other.rx_bytes
	stats.err_tx_drops += other.err_tx_drops

	other_histogram := hdrhistogram.Import(other.snapshot)
	stats_histogram := hdrhistogram.Import(stats.snapshot)
	stats_histogram.Merge(other_histogram)
	stats.snapshot = stats_histogram.Export()
}

// Function to return the difference between two stats.
func (curr *stats) diff_stats(other *stats) stats {
	var diff stats
	diff.tx_success = curr.tx_success - other.tx_success
	diff.tx_bytes = curr.tx_bytes - other.tx_bytes
	diff.rx_count = curr.rx_count - other.rx_count
	diff.rx_bytes = curr.rx_bytes - other.rx_bytes
	diff.err_tx_drops = curr.err_tx_drops - other.err_tx_drops

	// Histogram does not depend on the previous stats.
	diff.snapshot = curr.snapshot
	return diff
}

func tx(task *task_ctx, stats *stats) {
	channel_ctx := task.ctx

	// Return if we have already sent the required number of messages.
	if msg_nr <= stats.tx_success {
		return
	}

	// Don't send more than msg_window messages at a time.
	if stats.tx_success-stats.rx_count >= uint64(msg_window) {
		return
	}

	ret := machnet.SendMsg(channel_ctx, task.flow, &task.msg_data[0], uint(msg_size))
	if ret == 0 {
		stats.tx_success += 1
		stats.tx_bytes += uint64(msg_size)
	} else {
		stats.err_tx_drops += 1
	}
}

func rx(task *task_ctx, stats *stats) {
	channel_ctx := task.ctx

	err, _ := machnet.RecvMsg(channel_ctx, &task.rx_msg[0], uint(msg_size))
	if err != 0 {
		return
	}

	// Verify if the received message is the same as the sent message.
	if verify {
		for i := 0; i < msg_size; i++ {
			if task.rx_msg[i] != task.msg_data[i] {
				glog.Fatalf("Received message does not match the sent message: %d %d %d", i, task.rx_msg[i], task.msg_data[i])
			}
		}
	}

	stats.rx_count += 1
	stats.rx_bytes += uint64(msg_size)
}

// Ping a message and wait for a response for latency measurements.
func ping(task *task_ctx, stats *stats, histogram *hdrhistogram.Histogram) {
	channel_ctx := task.ctx

	// Return if we have already sent the required number of messages.
	if msg_nr <= stats.tx_success {
		return
	}

	// Start timer.
	start := time.Now()

	ret := machnet.SendMsg(channel_ctx, task.flow, &task.msg_data[0], uint(msg_size))
	if ret == 0 {
		stats.tx_success += 1
		stats.tx_bytes += uint64(msg_size)
	} else {
		stats.err_tx_drops += 1
	}

	// Keep reading until we get a message from the same flow.
	err, _ := machnet.RecvMsg(channel_ctx, &task.rx_msg[0], uint(msg_size))
	for err != 0 {
		err, _ = machnet.RecvMsg(channel_ctx, &task.rx_msg[0], uint(msg_size))
	}

	// Stop timer.
	elapsed := time.Since(start)
	histogram.RecordValue(elapsed.Nanoseconds())

	// Verify if the received message is the same as the sent message.
	if verify {
		for i := 0; i < msg_size; i++ {
			if task.rx_msg[i] != task.msg_data[i] {
				glog.Fatalf("Received message does not match the sent message: %d %d %d", i, task.rx_msg[i], task.msg_data[i])
			}
		}
	}

	stats.rx_count += 1
	stats.rx_bytes += uint64(msg_size)
}

func bounce(task *task_ctx, stats *stats) {
	channel_ctx := task.ctx

	err, flow_info := machnet.RecvMsg(channel_ctx, &task.rx_msg[0], uint(msg_size))
	if err != 0 {
		return
	}
	stats.rx_count += 1
	stats.rx_bytes += uint64(msg_size)

	// Swap the source and destination IP addresses.
	tmp_flow := flow_info
	flow_info.SrcIp = tmp_flow.DstIp
	flow_info.DstIp = tmp_flow.SrcIp
	flow_info.SrcPort = tmp_flow.DstPort
	flow_info.DstPort = tmp_flow.SrcPort

	// Prepare to send back the message.
	ret := machnet.SendMsg(channel_ctx, flow_info, &task.rx_msg[0], uint(msg_size))
	if ret == 0 {
		stats.tx_success += 1
		stats.tx_bytes += uint64(msg_size)
	} else {
		stats.err_tx_drops += 1
	}
}

func run_worker(task *task_ctx, active_generator bool, tick *time.Ticker, stats_chan chan<- stats) {
	task_stats := new_stats()
	count_histogram := hdrhistogram.New(1, 1000000, 3)
	if active_generator {
		for {
			if latency {
				ping(task, task_stats, count_histogram)
			} else {
				tx(task, task_stats)
				rx(task, task_stats)
			}

			// Report the stats every second.
			select {
			case <-tick.C:
				if latency {
					task_stats.snapshot = count_histogram.Export()
					count_histogram.Reset()
				}
				stats_chan <- *task_stats
			default:
				continue
			}
		}
	} else {
		for {
			bounce(task, task_stats)

			// Report the stats every second.
			select {
			case <-tick.C:
				stats_chan <- *task_stats
			default:
				continue
			}
		}
	}
}

func main() {
	init_flags()

	if active_generator {
		glog.Info("Starting in active generator mode.")
	} else {
		glog.Info("Starting in passive message bouncing mode.")
	}

	// Initialize the machnet library.
	ret := machnet.Init()
	if ret != 0 {
		glog.Fatal("Failed to initialize the machnet library.")
	}

	// Define a pointer variable channel_ctx to store the output of C.machnet_attach()
	var channel_ctx *machnet.MachnetChannelCtx = machnet.Attach() // TODO: Defer machnet.Detach()?

	if channel_ctx == nil {
		glog.Fatal("Failed to attach to the channel.")
	}

	// Read the contents of file config_json into a byte array.
	json_bytes, err := os.ReadFile(config_json)
	if err != nil {
		glog.Fatal("Failed to read config file.")
	}

	// Parse the json file to get the local_ip.
	local_ip, _ := jsonparser.GetString(json_bytes, "hosts_config", local_hostname, "ipv4_addr")

	var flow machnet.MachnetFlow
	if active_generator {
		// Parse the json file to get the remote_ip.
		remote_ip, _ := jsonparser.GetString(json_bytes, "hosts_config", remote_hostname, "ipv4_addr")

		// Initiate connection to the remote host.
		ret, flow = machnet.Connect(channel_ctx, local_ip, remote_ip, remote_port)
		if ret != 0 {
			glog.Fatal("Failed to connect to remote host.")
		}
		glog.Info("[CONNECTED] [", local_ip, " <-> ", remote_ip, ":", remote_port, "]")
	} else {
		// Listen for incoming connections.
		ret = machnet.Listen(channel_ctx, local_ip, remote_port)
		if ret != 0 {
			glog.Fatal("Failed to listen for incoming connections.")
		}
		glog.Info("[LISTENING] [", local_ip, ":", remote_port, "]")
	}

	// Create a task context for all CPU cores.
	task_ctx := new_task_ctx(channel_ctx, flow)

	// Number of CPU cores to run on.
	cpu_cores := 1

	// Create a channel to receive the stats.
	stats_chan := make(chan stats, cpu_cores)
	var wg sync.WaitGroup
	wg.Add(cpu_cores)

	for w := 1; w <= cpu_cores; w++ {
		// Add ticker to print stats.
		ticker := time.NewTicker(1 * time.Second)
		defer ticker.Stop()
		go run_worker(task_ctx, active_generator, ticker, stats_chan)
	}

	// Print the stats.
	go func() {
		accumulated_stats := new_stats() // Accumulate the stats from all CPU cores.
		var prev_stats stats             // Hold the previously accumulated stats.
		var report_stats stats           // Hold the difference between the current and previous stats.
		for {
			accumulated_stats = new_stats()
			for i := 0; i < cpu_cores; i++ {
				s := <-stats_chan
				accumulated_stats.accumulate_stats(&s)
			}
			report_stats = accumulated_stats.diff_stats(&prev_stats)
			report_stats.report_stats()
			prev_stats = *accumulated_stats
		}
	}()

	wg.Wait()
}
