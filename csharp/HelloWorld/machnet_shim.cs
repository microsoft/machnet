using System;
using System.Runtime.InteropServices;
using System.Text;

public struct NSaaSNetFlow_t
{
    public UInt32 src_ip;
    public UInt32 dst_ip;
    public UInt16 src_port;
    public UInt16 dst_port;
}

public static class NSaaSShim
{
    private const string libnsaas_shim_location = "libnsaas_shim.so";

    [DllImport(libnsaas_shim_location, CallingConvention = CallingConvention.Cdecl)]
    public static extern int nsaas_init();

    [DllImport(libnsaas_shim_location, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr nsaas_attach();

    [DllImport(libnsaas_shim_location, CallingConvention = CallingConvention.Cdecl)]
    public static extern int nsaas_listen(IntPtr channel_ctx, string local_ip, UInt16 port);

    [DllImport(libnsaas_shim_location, CallingConvention = CallingConvention.Cdecl)]
    public static extern int nsaas_connect(IntPtr channel_ctx, string local_ip, string remote_ip, UInt16 port, ref NSaaSNetFlow_t flow);

    [DllImport(libnsaas_shim_location, CallingConvention = CallingConvention.Cdecl)]
    public static extern int nsaas_send(IntPtr channel_ctx, NSaaSNetFlow_t flow, byte[] data, IntPtr dataSize);

    [DllImport(libnsaas_shim_location, CallingConvention = CallingConvention.Cdecl)]
    public static extern int nsaas_recv(IntPtr channel_ctx, byte[] data, IntPtr dataSize, ref NSaaSNetFlow_t flow);
}