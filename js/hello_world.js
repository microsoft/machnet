// A simple example of using NSaaS that sends the message "Hello World!" over
// the network.
//
// Requirements: npm install ref-napi ffi-napi ref-struct-napi
//
// Usage: Assuming we have two servers (A and B), where NSaaS is running on both
// IP 10.0.255.100 at server A, and IP 10.0.255.101 at server B.
//
// On server A: node hello_world.js --local_ip 10.0.255.100
// On server B: node hello_world.js --local_ip 10.0.255.101 --remote_ip 10.0.255.100
//
// If everything works, server A should print "Received message: Hello World!"

const ref = require('ref-napi');
const commander = require('commander');
const chalk = require('chalk');
const {nsaas_shim, NSaaSNetFlow_t} = require('./nsaas_shim');

function customCheck(condition, message) {
  if (!condition) {
    console.log(chalk.red('Error: ' + message));
    process.exit(1);
  } else {
    console.log(chalk.green('Success: ' + message));
  }
}

const kHelloWorldPort = 888;
commander.option('-l, --local_ip <ip>', 'Local IP address')
    .option('-r, --remote_ip <ip>', 'Remote IP address')
    .parse(process.argv);

const options = commander.opts();
if (!options.local_ip) {
  console.log(chalk.red('Error: local_ip is required'));
  process.exit(1);
}
console.log(options);

// Main logic
var ret = nsaas_shim.nsaas_init();
customCheck(ret === 0, 'nsaas_init()');

var channel_ctx = nsaas_shim.nsaas_attach();
customCheck(channel_ctx !== null, 'nsaas_attach()');

if (options.remote_ip) {
  // Client
  const flow = new NSaaSNetFlow_t();
  ret = nsaas_shim.nsaas_connect(
      channel_ctx, ref.allocCString(options.local_ip),
      ref.allocCString(options.remote_ip), kHelloWorldPort, flow.ref());
  customCheck(ret === 0, 'nsaas_connect()');

  const msg = 'Hello World!';
  const msg_buffer = Buffer.from(msg, 'utf8');
  ret = nsaas_shim.nsaas_send(channel_ctx, flow, msg_buffer, msg_buffer.length);
  if (ret === -1) {
    console.log(chalk.red('Error: nsaas_send() failed'));
  } else {
    console.log(chalk.green('Message sent successfully'));
  }
} else {
  // Server
  console.log('Waiting for message from client');
  ret = nsaas_shim.nsaas_listen(
      channel_ctx, ref.allocCString(options.local_ip), kHelloWorldPort);
  customCheck(ret === 0, 'nsaas_listen()');

  function receive_message() {
    const buf = Buffer.alloc(1024);
    const flow = new NSaaSNetFlow_t();
    const bytesRead =
        nsaas_shim.nsaas_recv(channel_ctx, buf, buf.length, flow.ref());

    if (bytesRead === -1) {
      console.log(chalk.red('Error: nsaas_recv() failed'));
    } else if (bytesRead === 0) {
      setTimeout(receive_message, 10);
    } else {
      const receivedMsg = buf.toString('utf8', 0, bytesRead);
      console.log(`Received message: ${receivedMsg}`);
      receive_message();
    }
  }

  receive_message();
}
