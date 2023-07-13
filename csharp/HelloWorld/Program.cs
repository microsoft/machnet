// Example hello world program for Machnet
// Usage: Assuming we have two servers (A and B), where Machnet is running on both
// IP 10.0.255.100 at server A, and IP 10.0.255.101 at server B.
//
// On server A: dotnet run --local_ip 10.0.255.100
// On server B: dotnet run --local_ip 10.0.255.101 --remote_ip 10.0.255.100
//
// If everything works, server A should print "Received message: Hello World!"

using System;
using System.Text;
using CommandLine;

class Program
{
    private const UInt16 kHelloWorldPort = 888;

    public class Options
    {
        [Option('l', "local_ip", Required = true, HelpText = "Local IP address")]
        public string? LocalIp { get; set; }

        [Option('r', "remote_ip", Required = false, HelpText = "Remote IP address")]
        public string? RemoteIp { get; set; }
    }

    static void CustomCheck(bool condition, string message)
    {
        if (!condition)
        {
            Console.ForegroundColor = ConsoleColor.Red;
            Console.WriteLine("Error: " + message);
            Environment.Exit(1);
        }
        else
        {
            Console.ForegroundColor = ConsoleColor.Green;
            Console.WriteLine("Success: " + message);
        }
        Console.ResetColor();
    }

    static void Main(string[] args)
    {
        Options? options = null;
        Parser.Default.ParseArguments<Options>(args)
            .WithParsed<Options>(o => options = o);

        if (options?.LocalIp == null)
        {
            Console.ForegroundColor = ConsoleColor.Red;
            Console.WriteLine("Error: Local IP address is required.");
            Console.ResetColor();
            Environment.Exit(1);
        }


        Console.WriteLine($"Local IP: {options.LocalIp}");
        if (!string.IsNullOrEmpty(options.RemoteIp))
        {
            Console.WriteLine($"Remote IP: {options.RemoteIp}");
        }

        int ret = MachnetShim.machnet_init();
        CustomCheck(ret == 0, "machnet_init()");

        IntPtr channel_ctx = MachnetShim.machnet_attach();
        CustomCheck(channel_ctx != IntPtr.Zero, "machnet_attach()");

        if (!string.IsNullOrEmpty(options.RemoteIp))
        {
            // Client
            MachnetFlow_t flow = new MachnetFlow_t();
            ret = MachnetShim.machnet_connect(channel_ctx, options.LocalIp, options.RemoteIp, kHelloWorldPort, ref flow);
            CustomCheck(ret == 0, "machnet_connect()");

            string msg = "Hello World!";
            byte[] msgBuffer = Encoding.UTF8.GetBytes(msg);
            ret = MachnetShim.machnet_send(channel_ctx, flow, msgBuffer, new IntPtr(msgBuffer.Length));
            CustomCheck(ret != -1, "machnet_send()");

            Console.WriteLine("Message sent successfully");
        }
        else
        {
            Console.WriteLine("Waiting for message from client");
            ret = MachnetShim.machnet_listen(channel_ctx, options.LocalIp, kHelloWorldPort);
            CustomCheck(ret == 0, "machnet_listen()");

            while (true)
            {
                byte[] buf = new byte[1024];
                MachnetFlow_t flow = new MachnetFlow_t();
                int bytesRead = MachnetShim.machnet_recv(channel_ctx, buf, new IntPtr(buf.Length), ref flow);

                if (bytesRead == -1)
                {
                    Console.ForegroundColor = ConsoleColor.Red;
                    Console.WriteLine("Error: machnet_recv() failed");
                    Console.ResetColor();
                    break;
                }
                else if (bytesRead == 0)
                {
                    SpinWait.SpinUntil(() => false, 10);
                }
                else
                {
                    string receivedMsg = Encoding.UTF8.GetString(buf, 0, bytesRead);
                    Console.WriteLine($"Received message: {receivedMsg}");
                }
            }
        }
    }
}
