#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
mod bindings {
    // include!(concat!(env!("MACHNET"), "/bindings.rs"));
    include!("bindings.rs");
}

pub use bindings::{MachnetChannelCtrlCtx, MachnetFlow};

use std::ffi::{c_void, CString};

impl MachnetChannelCtrlCtx {
    unsafe fn from_raw(ptr: *mut c_void) -> Option<Self> {
        let typed_ptr = ptr as *mut MachnetChannelCtrlCtx;
        if typed_ptr.is_null() {
            None
        } else {
            Some(*typed_ptr)
            // typed_ptr.as_ref();
        }
    }

    // only for demonstration purposes
    // channel context should be initialized using machnet_attach()
    pub fn default() -> Self {
        MachnetChannelCtrlCtx {
            // default values
            req_id: 0,
        }
    }

    fn new() -> Self {
        todo!()
    }
}

impl MachnetFlow {
    fn default() -> Self {
        MachnetFlow {
            src_ip: 0,
            dst_ip: 0,
            src_port: 0,
            dst_port: 0,
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
/// let control_ctx = machnet_attach();
/// match control_ctx {
///     Some(ctx) => {
///         // Successfully attached to Machnet channel, use `ctx` here
///     }
///     None => {
///         // Handle the case where attachment to Machnet channel fails
///     }
/// }
/// ```
/// # Returns
/// Returns an ```Option<MachnetChannelCtrlCtx>```.
///
pub fn machnet_attach() -> Option<MachnetChannelCtrlCtx> {
    unsafe {
        let ptr = bindings::machnet_attach();
        MachnetChannelCtrlCtx::from_raw(ptr)
    }
}

/// Establishes a Machnet connection using the provided control context.
///
/// This function attempts to create a connection between a local IP address and a remote IP address on a specified port.
///
/// # Arguments
///
/// * `ctx` - A mutable reference to the `MachnetChannelCtrlCtx` which holds
///   the control context for the Machnet connection.
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
/// use machnet::{MachnetChannelCtrlCtx, machnet_connect,machnet_attach};
///
/// // Example context initialization, normally you do machnet_attach() to get the context
/// let mut ctx = MachnetChannelCtrlCtx::default();
/// let local_ip = "192.168.1.2";
/// let remote_ip = "192.168.1.3";
/// let remote_port = 8080;
///
/// match machnet_connect(&mut ctx, local_ip, remote_ip, remote_port) {
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
    ctx: &MachnetChannelCtrlCtx,
    local_ip: &str,
    remote_ip: &str,
    remote_port: u16,
) -> Option<MachnetFlow> {
    unsafe {
        let ctx_ptr = ctx as *const _ as *mut c_void;
        let mut flow = MachnetFlow::default();
        // needed as local variables due to lifetime reasons
        let local_ip_cstr = CString::new(local_ip).unwrap();
        let remote_ip_cstr = CString::new(remote_ip).unwrap();

        let res = bindings::machnet_connect(
            ctx_ptr,
            local_ip_cstr.as_ptr(),
            remote_ip_cstr.as_ptr(),
            remote_port,
            &mut flow,
        );

        match res {
            -1 => None,
            _ => Some(flow),
        }
    }
}
