// Copyright (C) 2023 Vahab Jabrayilov
// Email: vjabrayilov@cs.columbia.edu
//
// This file is part of the Machnet project.
//
// This project is licensed under the MIT License - see the LICENSE file for details

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
mod bindings {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

pub use bindings::MachnetFlow;

use std::{
    ffi::{c_void, CString},
    marker::PhantomData,
};

#[derive(Debug)]
pub struct MachnetChannel<'a> {
    ptr: *mut c_void,
    _marker: PhantomData<&'a mut c_void>,
}

impl<'a> MachnetChannel<'a> {
    pub fn new(ptr: *mut c_void) -> Self {
        MachnetChannel {
            ptr,
            _marker: PhantomData,
        }
    }

    pub fn from_raw(ptr: *mut c_void) -> Option<Self> {
        if ptr.is_null() {
            None
        } else {
            Some(MachnetChannel::new(ptr))
        }
    }

    pub fn get_ptr(&self) -> *mut c_void {
        self.ptr
    }
}

unsafe impl<'a> Send for MachnetChannel<'a> {}

impl MachnetFlow {
    pub fn default() -> Self {
        MachnetFlow {
            src_ip: 0,
            dst_ip: 0,
            src_port: 0,
            dst_port: 0,
        }
    }

    pub fn new(src_ip: u32, src_port: u16, dst_ip: u32, dst_port: u16) -> Self {
        MachnetFlow {
            src_ip,
            dst_ip,
            src_port,
            dst_port,
        }
    }
}

/// Initializes the Machnet library for interacting with the Machnet sidecar.
/// This function sets up the necessary components to allow communication and control
/// over the Machnet sidecar.
///  It should be called before making any other calls to the  Machnet library.
///
/// /// # Examples
///
/// Basic usage:
///
/// ```
/// use machnet::machnet_init;
///
/// let result = machnet_init();
///
/// if result == 0 {
///     println!("Machnet initialized successfully.");
/// } else {
///     println!("Failed to initialize Machnet.");
/// }
/// ```
///
/// # Returns
///
/// Returns `0` on successful initialization of the Machnet library.
/// Returns `-1` if the initialization fails.
///
pub fn machnet_init() -> i32 {
    unsafe { bindings::machnet_init() }
}

/// Creates a new channel to the Machnet controller and binds to it.
/// A channel is a logical entity between an application and the Machnet service.
///
/// # Examples
///
/// ```
/// use machnet::machnet_attach;
///     
/// let channel= machnet_attach();
/// match channel{
///     Some(ch) => {
///         // Successfully attached to Machnet channel, use `ch` here
///     }
///     None => {
///         // Handle the case where attachment to Machnet channel fails
///     }
/// }
/// ```
/// # Returns
/// Returns an ```Option<MachnetChannel>```.
///
pub fn machnet_attach<'a>() -> Option<MachnetChannel<'a>> {
    unsafe {
        let ptr = bindings::machnet_attach();
        MachnetChannel::from_raw(ptr)
    }
}

/// Establishes a Machnet connection using the provided chanenel.
///
/// This function attempts to create a connection between a local IP address and a remote IP address on a specified port.
///
/// # Arguments
///
/// * `channel` - A mutable reference to the `MachnetChannel`.
/// * `local_ip` - A string slice representing the local IP address to bind to.
/// * `remote_ip` - A string slice representing the remote IP address to connect to.
/// * `remote_port` - The remote port number to connect to.
///
/// # Returns
///
/// Returns an `Option<MachnetFlow>`
//
/// # Examples
///
/// ```
/// use machnet::{machnet_connect,machnet_attach};
///
/// let mut channel = machnet_attach().unwrap();
/// let local_ip = "192.168.1.2";
/// let remote_ip = "192.168.1.3";
/// let remote_port = 8080;
///
/// match machnet_connect(&mut channel, local_ip, remote_ip, remote_port) {
///     Some(flow) => {
///         // Connection was successful, use `flow` here
///     }
///     None => {
///         // Connection failed
///     }
/// }
/// ```
///
pub fn machnet_connect(
    channel: &MachnetChannel,
    local_ip: &str,
    remote_ip: &str,
    remote_port: u16,
) -> Option<MachnetFlow> {
    unsafe {
        let channel_ptr = channel.get_ptr();
        let mut flow = MachnetFlow::default();
        let flow_ptr = &mut flow as *mut MachnetFlow;
        // needed as local variables due to lifetime reasons
        let local_ip_cstr = CString::new(local_ip).unwrap();
        let remote_ip_cstr = CString::new(remote_ip).unwrap();
        let res = bindings::machnet_connect(
            channel_ptr,
            local_ip_cstr.as_ptr(),
            remote_ip_cstr.as_ptr(),
            remote_port,
            flow_ptr, // &mut flow,
        );

        match res {
            -1 => None,
            _ => Some(flow),
        }
    }
}

/// Listens for incoming messages on a specified local IP address and port.
///
/// This function establishes a listener on the given `local_ip` and `local_port`
/// using the provided `MachnetChannel`.
///
/// # Arguments
///
/// * `channel` - A reference to the `MachnetChannel` associated with the channel
///   that will be used for listening.
/// * `local_ip` - A string slice representing the local IP address to listen on.
///   This should be a valid IPv4 or IPv6 address.
/// * `local_port` - The local port number to listen on.
///  This should be a valid port   that is not already in use.
///
/// # Returns
///
/// Returns `0` on successful setup, `-1` on failure.
///
/// # Examples
///
/// Basic usage:
///
/// ```
/// use machnet::{machnet_attach, machnet_listen};
///
/// let mut channel = machnet_attach().unwrap();
/// let local_ip = "127.0.0.1";
/// let local_port = 8080;
///
/// let result = machnet_listen(&mut channel, local_ip, local_port);
/// if result == 0 {
///     println!("Listening on {}:{}", local_ip, local_port);
/// } else {
///     println!("Failed to set up listener on {}:{}", local_ip, local_port);
/// }
/// ```
///
pub fn machnet_listen(channel: &mut MachnetChannel, local_ip: &str, local_port: u16) -> i32 {
    unsafe {
        let channel_ptr = channel.get_ptr();
        let local_ip_cstr = CString::new(local_ip).unwrap();
        bindings::machnet_listen(channel_ptr, local_ip_cstr.as_ptr(), local_port)
    }
}

/// Enqueues a message for transmission to a remote peer over the network.
///
/// This function sends data over a specified Machnet channel.
/// It uses the provided
/// Machnet channel context and a pre-created flow to the remote peer.
/// The data to be sent is specified by a buffer and its length in bytes.
///
/// # Arguments
///
/// * `channel` - A reference to the `MachnetChannel` representing the Machnet channel.
/// * `flow` - The `MachnetFlow` instance representing a pre-created flow to the remote peer.
/// * `buf` - A byte slice (`&[u8]`) reference representing the data buffer to be sent to the remote peer.
/// * `len` - The length of the data buffer in bytes (type `u64`).
///
/// # Returns
///
/// Returns `0` on successful transmission, `-1` on failure.
///
/// # Examples
///
/// Basic usage:
///
/// ```
/// # use machnet::{MachnetChannel, MachnetFlow, machnet_send, machnet_attach};
/// // Following is just for demonstration purposes, normally you do machnet_attach() to get the context and machnet_connect() to get the flow
/// let mut channel = machnet_attach().unwrap();
/// let flow = MachnetFlow::default();
/// let data = [1, 2, 3, 4]; // Example data to send
/// let len = data.len() as u64;
///
/// let result = machnet_send(&mut channel, flow, &data, len);
/// if result == 0 {
///     println!("Message enqueued for transmission");
/// } else {
///     println!("Failed to enqueue message");
/// }
/// ```
///
pub fn machnet_send(channel: &mut MachnetChannel, flow: MachnetFlow, buf: &[u8], len: u64) -> i32 {
    unsafe {
        let channel_ptr = channel.get_ptr();
        let buf_ptr = buf.as_ptr() as *const c_void;
        bindings::machnet_send(channel_ptr, flow, buf_ptr, len as usize)
    }
}

/// Receives a pending message from a remote peer over the network.
/// This function attempts to receive data from a specified Machnet channel. It uses the provided
/// Machnet channel (`channel`) and fills the given buffer (`buf`) with the received data.
/// The `flow` parameter will be populated with the flow information of the sender.
///
/// # Arguments
///
/// * `channel` - A reference to the `MachnetChannel` representing the Machnet channel.
/// * `buf` - A mutable byte slice (`&mut [u8]`) that will be filled with the received message.
///   The length of `buf` should be at least as large as the `len` parameter.
/// * `len` - The length of `buf` in bytes. This indicates the maximum amount of data
///   that can be written into `buf`.
/// * `flow` - A reference to `MachnetFlow` where the flow information of the sender will be stored.
///
/// # Returns
///
/// Returns a `i64` indicating the result of the receive operation:
/// * `0` if no message is currently available.
/// * `-1` on failure, such as if an error occurs during the receive operation.
/// * Otherwise, returns the number of bytes received and written into `buf`.
///
/// # Examples
///
/// Basic usage:
///
/// ```
/// use machnet::{ MachnetFlow, machnet_attach, machnet_recv};
/// // Following is just for demonstration purposes, normally you do machnet_attach() to get the context and flow is obtained from machnet_listen().
/// let mut channel = machnet_attach().unwrap();
/// let mut flow = MachnetFlow::default();
/// let mut buffer = [0; 1024]; // Example buffer
/// let len = buffer.len() as u64;
///
/// match machnet_recv(&channel, &mut buffer, len, &mut flow) {
///     0 => println!("No message available"),
///     -1 => println!("Failed to receive message"),
///     bytes_received => println!("Received {} bytes", bytes_received),
/// }
/// ```
///
pub fn machnet_recv(
    channel: &MachnetChannel,
    buf: &mut [u8],
    len: u64,
    flow: &mut MachnetFlow,
) -> i64 {
    // TODO(vjabrayilov): Refactor so that flow is also returned value.
    unsafe {
        let channel_ptr = channel.get_ptr();
        let buf_ptr = buf.as_ptr() as *mut c_void;
        let flow_ptr = flow as *mut MachnetFlow;
        bindings::machnet_recv(channel_ptr, buf_ptr, len as usize, flow_ptr) as i64
    }
}
