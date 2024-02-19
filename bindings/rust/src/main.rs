use clap::{Parser, Subcommand};
use lazy_static::lazy_static;
use log::{info, warn};
use machnet::machnet_init;
use signal_hook::{consts::SIGINT, iterator::Signals};
use std::mem::size_of;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
const ABOUT: &str = "This application is a simple message generator that supports sending and receiving network messages using Machnet.";

lazy_static! {
    static ref UINT64_MAX_STR: String = u64::MAX.to_string();
}

#[derive(Parser)]
#[command(name="msg_gen", version = "1.0", about =ABOUT, long_about = None)]
struct MsgGen {
    #[arg(short, long, action = clap::ArgAction::Count)]
    debug: u8,

    #[structopt(long, help = "IP of the local Machnet interface")]
    local_ip: String,

    #[structopt(
        long,
        default_value = "",
        help = "IP of the remote server's Machnet interface"
    )]
    remote_ip: String,

    #[structopt(long, default_value = "888", help = "Remote port to connect to")]
    remote_port: u32,

    #[structopt(long, default_value = "888", help = "Local port to connect to")]
    local_port: u32,

    #[structopt(
        long,
        default_value = "64",
        help = "Size of the message (request/response) to send"
    )]
    msg_size: u32,

    #[structopt(
        long,
        default_value = "8",
        help = "Maximum number of messages in flight"
    )]
    msg_window: u32,

    #[structopt(
        long,
        default_value=&**UINT64_MAX_STR,
        help = "Number of messages to send"
    )]
    msg_nr: u64,

    #[structopt(long, help = "Verify payload of received messages")]
    verify: bool,
}

static G_KEEP_RUNNING: AtomicBool = AtomicBool::new(true);

struct AppHdr {
    window_slot: u64,
}

struct Stats {
    tx_success: u64,
    tx_bytes: u64,
    rx_count: u64,
    rx_bytes: u64,
    err_tx_drops: u64,
}

impl Stats {
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

fn setup_signal_handler() {
    let mut signals = Signals::new(&[SIGINT]).unwrap();

    thread::spawn(move || {
        for _sig in signals.forever() {
            warn!("Signal received!");
            G_KEEP_RUNNING.store(false, Ordering::SeqCst);
        }
    });
}

fn main() {
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

    assert_eq!(machnet_init(), 0, "Failed to initialize Machnet library.");
}
