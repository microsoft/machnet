using System;
using System.Runtime.InteropServices;
using System.Text;

public struct MachnetFlow_t
{
    public UInt32 src_ip;
    public UInt32 dst_ip;
    public UInt16 src_port;
    public UInt16 dst_port;
}

public static class MachnetShim
{
    private const string libmachnet_shim_location = "libmachnet_shim.so";

    [DllImport(libmachnet_shim_location, CallingConvention = CallingConvention.Cdecl)]
    public static extern int machnet_init();

    [DllImport(libmachnet_shim_location, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr machnet_attach();

    [DllImport(libmachnet_shim_location, CallingConvention = CallingConvention.Cdecl)]
    public static extern int machnet_listen(IntPtr channel_ctx, string local_ip, UInt16 port);

    [DllImport(libmachnet_shim_location, CallingConvention = CallingConvention.Cdecl)]
    public static extern int machnet_connect(IntPtr channel_ctx, string local_ip, string remote_ip, UInt16 port, ref MachnetFlow_t flow);

    [DllImport(libmachnet_shim_location, CallingConvention = CallingConvention.Cdecl)]
    public static extern int machnet_send(IntPtr channel_ctx, MachnetFlow_t flow, byte[] data, IntPtr dataSize);

    [DllImport(libmachnet_shim_location, CallingConvention = CallingConvention.Cdecl)]
    public static extern int machnet_recv(IntPtr channel_ctx, byte[] data, IntPtr dataSize, ref MachnetFlow_t flow);
}
