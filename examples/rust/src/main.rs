use clap::Parser;
use hdrhistogram::Histogram;
use log::{debug, error, info, warn};
use machnet::{
    machnet_attach, machnet_connect, machnet_listen, machnet_recv, machnet_send, MachnetChannel,
    MachnetFlow,
};
use signal_hook::{consts::SIGINT, iterator::Signals};
use std::env;
use std::mem::size_of;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::{self, sleep};
use std::time::{Duration, Instant};

const ABOUT: &str = "This application is a simple message generator that supports sending and receiving network messages using Machnet.";

#[derive(Parser, Debug)]
#[command(name="msg_gen", version = "1.0", about =ABOUT, long_about = None)]
struct MsgGen {
    // #[arg(short, long, action = clap::ArgAction::Count)]
    // debug: u8,
    #[arg(long, help = "IP of the local Machnet interface")]
    local_ip: String,

    #[arg(long, default_value_t = String::new(), help= "IP of the remote server's Machnet interface, if left empty starts in server mode.")]
    remote_ip: String,

    #[arg(long, default_value_t = 888, help = "Remote port to connect to")]
    remote_port: u16,

    #[arg(long, default_value_t = 888, help = "Local port to connect to")]
    local_port: u16,

    #[arg(
        long,
        default_value_t = 64,
        help = "Size of the message (request/response) to send"
    )]
    msg_size: u64,

    #[arg(
        long,
        default_value_t = 8,
        help = "Maximum number of messages in flight"
    )]
    msg_window: u64,

    #[arg(
        long,
        default_value_t = u64::MAX,
        help = "Number of messages to send"
    )]
    msg_nr: u64,

    #[arg(long, help = "Verify payload of received messages")]
    verify: bool,
}

static G_KEEP_RUNNING: AtomicBool = AtomicBool::new(true);

struct AppHdr {
    window_slot: u64,
}
#[derive(Debug, Clone, Copy)]
struct StatsInstance {
    tx_success: u64,
    tx_bytes: u64,
    rx_count: u64,
    rx_bytes: u64,
    err_tx_drops: u64,
}

impl StatsInstance {
    fn new() -> Self {
        Self {
            tx_success: 0,
            tx_bytes: 0,
            rx_count: 0,
            rx_bytes: 0,
            err_tx_drops: 0,
        }
    }
}

struct Stats {
    current: StatsInstance,
    prev: StatsInstance,
    last_measurement_time: Instant,
}

impl Stats {
    fn new() -> Self {
        Self {
            current: StatsInstance::new(),
            prev: StatsInstance::new(),
            last_measurement_time: Instant::now(),
        }
    }
}
#[derive(Debug, Clone)]
struct MsgLatencyInfo {
    tx_ts: Instant,
}
struct ThreadCtx<'a> {
    channel_ctx: MachnetChannel<'a>,
    flow: Option<MachnetFlow>,
    rx_message: Vec<u8>,
    tx_message: Vec<u8>,
    message_gold: Vec<u8>,
    latency_hist: hdrhistogram::Histogram<u64>,
    num_request_latency_samples: u64,
    msg_latency_info_vec: Vec<MsgLatencyInfo>,
    stats: Stats,
}

impl<'a> ThreadCtx<'a> {
    const MIN_LATENCY_MICROS: u64 = 1;
    const MAX_LATENCY_MICROS: u64 = 100 * 100 * 100; //100 sec
    const LATENCY_PRECISION: u8 = 2; // two significant digits
    const MACHNET_MSG_MAX_LEN: usize = (8 * 1 << 20); // 8MB

    fn new(channel_ctx: MachnetChannel<'a>, flow: Option<MachnetFlow>, msg_window: u64) -> Self {
        let histogram = Histogram::<u64>::new_with_bounds(
            Self::MIN_LATENCY_MICROS,
            Self::MAX_LATENCY_MICROS,
            Self::LATENCY_PRECISION,
        )
        .expect("Failed to initialize latency histogram.");
        Self {
            channel_ctx,
            flow,
            rx_message: vec![0; Self::MACHNET_MSG_MAX_LEN],
            tx_message: vec![0; Self::MACHNET_MSG_MAX_LEN],
            message_gold: vec![0; Self::MACHNET_MSG_MAX_LEN],
            latency_hist: histogram,
            num_request_latency_samples: 0,
            msg_latency_info_vec: vec![
                MsgLatencyInfo {
                    tx_ts: Instant::now()
                };
                msg_window.try_into().unwrap()
            ],
            stats: Stats::new(),
        }
    }

    fn record_request_start(&mut self, window_slot: usize) {
        self.msg_latency_info_vec[window_slot].tx_ts = Instant::now();
    }

    fn record_request_end(&mut self, window_slot: usize) -> u64 {
        let msg_latency_info = &self.msg_latency_info_vec[window_slot];
        let latency_us = (Instant::now() - msg_latency_info.tx_ts).as_micros() as u64;

        self.latency_hist.record(latency_us).unwrap();
        self.num_request_latency_samples += 1;
        return latency_us;
    }
}

fn setup_signal_handler() {
    let mut signals = Signals::new(&[SIGINT]).unwrap();

    thread::spawn(move || {
        for _sig in signals.forever() {
            warn!("Signal received!");
            G_KEEP_RUNNING.store(false, Ordering::SeqCst);
        }
    });
}

fn report_stats(thread_ctx: &mut ThreadCtx) {
    let now = Instant::now();
    let sec_elapsed = now
        .duration_since(thread_ctx.stats.last_measurement_time)
        .as_secs();

    let cur = &thread_ctx.stats.current;
    let prev = &thread_ctx.stats.prev;

    if sec_elapsed >= 1 {
        let msg_sent = (cur.tx_success - prev.tx_success) as f64;
        let tx_kmps = msg_sent / (1_000.0 * sec_elapsed as f64);
        let tx_gbps = ((cur.tx_bytes - prev.tx_bytes) as f64 * 8.0) / (sec_elapsed as f64 * 1e9);

        let msg_received = (cur.rx_count - prev.rx_count) as f64;
        let rx_kmps = msg_received / (1_000.0 * sec_elapsed as f64);
        let rx_gbps = ((cur.rx_bytes - prev.rx_bytes) as f64 * 8.0) / (sec_elapsed as f64 * 1e9);

        let msg_dropped = cur.err_tx_drops - prev.err_tx_drops;

        let mut latency_stats_str = String::new();
        latency_stats_str.push_str("RTT (p50/99/99.9 us): ");

        if thread_ctx.num_request_latency_samples > 0 {
            let histogram = &thread_ctx.latency_hist;
            latency_stats_str.push_str(
                format!(
                    "{}/{}/{}",
                    histogram.value_at_quantile(0.5),
                    histogram.value_at_quantile(0.99),
                    histogram.value_at_quantile(0.999)
                )
                .as_str(),
            );
        } else {
            latency_stats_str.push_str("N/A");
        }

        let mut drop_stats_str = String::new();
        if msg_dropped > 0 {
            drop_stats_str.push_str(format!(", TX drops: {}", msg_dropped).as_str())
        }

        info!(
            "TX/RX (msg/sec, Gbps): ({:.1}K/{:.1}K, {:.3}/{:.3}). {}{}",
            tx_kmps, rx_kmps, tx_gbps, rx_gbps, latency_stats_str, drop_stats_str
        );

        thread_ctx.latency_hist.reset();
        thread_ctx.stats.last_measurement_time = now;
        thread_ctx.stats.prev = thread_ctx.stats.current;
    }
}

fn server(channel_ctx: MachnetChannel, msg_size: u64, msg_window: u64) {
    let mut thread_ctx = ThreadCtx::new(channel_ctx, None, msg_window);
    info!("Server: Starting.");

    loop {
        if !G_KEEP_RUNNING.load(Ordering::SeqCst) {
            info!("MsgGen: Exiting.");
            break;
        }

        let stats_cur = &mut thread_ctx.stats.current;
        let channel_ctx = &mut thread_ctx.channel_ctx;

        let mut rx_flow = MachnetFlow::default();
        let rx_message_size = thread_ctx.rx_message.len() as u64;
        let rx_size = machnet_recv(
            channel_ctx,
            &mut thread_ctx.rx_message,
            rx_message_size,
            &mut rx_flow,
        );

        if rx_size <= 0 {
            continue;
        }

        stats_cur.rx_count += 1;
        stats_cur.rx_bytes += rx_size as u64;

        // TOOD(vjabrayilov): remove this unsafe once we have a working msg_gen
        let req_hdr = unsafe { &*(thread_ctx.rx_message.as_ptr() as *const AppHdr) };
        let window_slot = req_hdr.window_slot as usize;
        debug!("Server: Received message with window slot {}", window_slot);

        let resp_hdr = unsafe { &mut *(thread_ctx.tx_message.as_ptr() as *mut AppHdr) };
        resp_hdr.window_slot = req_hdr.window_slot;

        // src_ip, src_port, dst_ip, dst_port
        let tx_flow = MachnetFlow::new(
            rx_flow.dst_ip,
            rx_flow.dst_port,
            rx_flow.src_ip,
            rx_flow.src_port,
        );

        let ret = machnet_send(channel_ctx, tx_flow, &thread_ctx.tx_message, msg_size);

        match ret {
            0 => {
                stats_cur.tx_success += 1;
                stats_cur.tx_bytes += msg_size;
            }
            _ => stats_cur.err_tx_drops += 1,
        }

        report_stats(&mut thread_ctx);
    }

    let stats_cur = thread_ctx.stats.current;
    log::info!(
    "Application Statistics (TOTAL) - [TX] Sent: {} ({} Bytes), Drops: {}, [RX] Received: {} ({} Bytes)",
    stats_cur.tx_success,
    stats_cur.tx_bytes,
    stats_cur.err_tx_drops,
    stats_cur.rx_count,
    stats_cur.rx_bytes);
}

fn client_send_one(thread_ctx: &mut ThreadCtx, msg_size: u64, window_slot: u64) {
    // debug!("Client: Sending message with window slot {}", window_slot);
    thread_ctx.record_request_start(window_slot as usize);
    let stats_cur = &mut thread_ctx.stats.current;
    // thread_ctx.msg_latency_info_vec[window_slot as usize].tx_ts = Instant::now();

    // TODO(vjabrayilov): remove this unsafe once we have a working msg_gen
    let req_hdr = unsafe { &mut *(thread_ctx.tx_message.as_ptr() as *mut AppHdr) };
    req_hdr.window_slot = window_slot;

    let ret = machnet_send(
        &mut thread_ctx.channel_ctx,
        thread_ctx.flow.unwrap(),
        &thread_ctx.tx_message,
        msg_size,
    );

    match ret {
        0 => {
            stats_cur.tx_success += 1;
            stats_cur.tx_bytes += msg_size;
        }
        _ => {
            warn!(
                "Client: Failed to send message with window slot {}",
                window_slot
            );
            stats_cur.err_tx_drops += 1;
        }
    }
}

fn client_recv_one_blocking(thread_ctx: &mut ThreadCtx, msg_window: u64) -> u64 {
    loop {
        if !G_KEEP_RUNNING.load(Ordering::SeqCst) {
            info!("client_recv_one_blocking: Exiting.");
            return 0;
        }

        let mut rx_flow = MachnetFlow::default();
        let rx_message_size = thread_ctx.rx_message.len() as u64;

        // new scope to mutably borrow `thread_ctx`
        {
            let channel_ctx = &mut thread_ctx.channel_ctx;
            let rx_size = machnet_recv(
                channel_ctx,
                &mut thread_ctx.rx_message,
                rx_message_size,
                &mut rx_flow,
            );

            if rx_size <= 0 {
                continue;
            }

            thread_ctx.stats.current.rx_count += 1;
            thread_ctx.stats.current.rx_bytes += rx_size as u64;
        }
        // TOOD(vjabrayilov): remove this unsafe once we have a working msg_gen
        let resp_hdr = unsafe { &*(thread_ctx.rx_message.as_ptr() as *const AppHdr) };
        if resp_hdr.window_slot >= msg_window {
            error!(
                "Client: Received message with invalid window slot {}",
                resp_hdr.window_slot
            );
            continue;
        }

        let latency_us = thread_ctx.record_request_end(resp_hdr.window_slot as usize);
        debug!(
            "Client: Received message with window slot {} and latency {} us",
            resp_hdr.window_slot, latency_us
        );

        // TODO(vjabrayilov): verify logic codes here
        // ideally, move all FLAGS_ to `thread_ctx` and simplify function signatures.
        // Code using `latency_us` goes here
        return resp_hdr.window_slot;
    }
}

fn client(channel_ctx: MachnetChannel, flow: MachnetFlow, msg_size: u64, msg_window: u64) {
    let mut thread_ctx = ThreadCtx::new(channel_ctx, Some(flow), msg_window);
    info!("Client: Starting.");

    for i in 0u64..msg_window {
        client_send_one(&mut thread_ctx, msg_size, i);
    }

    loop {
        if !G_KEEP_RUNNING.load(Ordering::SeqCst) {
            info!("MsgGen: Exiting.");
            break;
        }

        let rx_window_slot = client_recv_one_blocking(&mut thread_ctx, msg_window);
        client_send_one(&mut thread_ctx, msg_size, rx_window_slot);

        report_stats(&mut thread_ctx);
    }

    let stats_cur = &thread_ctx.stats.current;
    log::info!("Application Statistics (TOTAL) - [TX] Sent: {} ({} Bytes), Drops: {}, [RX] Received: {} ({} Bytes)", stats_cur.tx_success, stats_cur.tx_bytes, stats_cur.err_tx_drops, stats_cur.rx_count, stats_cur.rx_bytes);
}

fn main() {
    env::set_var("RUST_LOG", "info");
    env_logger::init();
    let msg_gen = MsgGen::parse();
    setup_signal_handler();

    assert!(
        msg_gen.msg_size > size_of::<AppHdr>().try_into().unwrap(),
        "Message size is too small."
    );

    if msg_gen.remote_ip.is_empty() {
        info!(
            "Starting in server mode, response size {}",
            msg_gen.msg_size
        );
    } else {
        info!("Starting in client mode, request size {}", msg_gen.msg_size);
    }

    assert_eq!(
        machnet::machnet_init(),
        0,
        "Failed to initialize Machnet library."
    );

    let mut channel_ctx = machnet_attach().unwrap();

    let datapath_thread = match msg_gen.remote_ip.is_empty() {
        true => {
            // Server-mode
            let ret = machnet_listen(&mut channel_ctx, &msg_gen.local_ip, msg_gen.local_port, 0);
            assert_eq!(ret, 0, "Failed to listen on local port.");

            info!("[LISTENING] [{}:{}]", msg_gen.local_ip, msg_gen.local_port);
            thread::spawn(move || server(channel_ctx, msg_gen.msg_size, msg_gen.msg_window))
        }
        false => {
            // Client-mode
            let flow = machnet_connect(
                &channel_ctx,
                &msg_gen.local_ip,
                &msg_gen.remote_ip,
                msg_gen.remote_port,
                0,
            )
            .expect("Failed to connect to remote host.");

            info!(
                "[CONNECTED] [{}:{} <-> {}:{}]",
                msg_gen.local_ip, flow.src_port, msg_gen.remote_ip, flow.dst_port
            );

            thread::spawn(move || client(channel_ctx, flow, msg_gen.msg_size, msg_gen.msg_window))
        }
    };

    while G_KEEP_RUNNING.load(Ordering::SeqCst) {
        sleep(Duration::from_secs(5));
    }
    datapath_thread.join().unwrap();
}
