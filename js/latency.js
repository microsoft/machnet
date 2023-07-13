const ref = require('ref-napi');
const commander = require('commander');
const chalk = require('chalk');
const {machnet_shim, MachnetFlow_t} = require('./machnet_shim');

const kHelloWorldPort = 888;
commander.option('-l, --local_ip <ip>', 'Local IP address')
    .option('-r, --remote_ip <ip>', 'Remote IP address')
    .option('-c, --is_client', 'Run as client')
    .parse(process.argv);

const options = commander.opts();
if (!options.local_ip || !options.remote_ip) {
  console.log(chalk.red('Error: local_ip and remote_ip are required'));
  process.exit(1);
}
console.log(options);

function customCheck(condition, message) {
  if (!condition) {
    console.log(chalk.red('Error: ' + message));
    process.exit(1);
  } else {
    console.log(chalk.green('Success: ' + message));
  }
}

function print_stats(arr) {
  const sorted = arr.sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)];
  const ninety = sorted[Math.floor(sorted.length * 0.9)];
  const ninety_nine = sorted[Math.floor(sorted.length * 0.99)];
  const ninety_nine_nine = sorted[Math.floor(sorted.length * 0.999)];

  console.log('Median: ' + Math.floor(median) + ' us');
  console.log('90th percentile: ' + Math.floor(ninety) + ' us');
  console.log('99th percentile: ' + Math.floor(ninety_nine) + ' us');
  console.log('99.9th percentile: ' + Math.floor(ninety_nine_nine) + ' us');
}

// Main logic
var ret = machnet_shim.machnet_init();
customCheck(ret === 0, 'machnet_init()');

var channel_ctx = machnet_shim.machnet_attach();
customCheck(channel_ctx !== null, 'machnet_attach()');

const NUM_MESSAGES = 100000;
const latencies_us = [];

const tx_flow = new MachnetFlow_t();
var ret = machnet_shim.machnet_connect(
    channel_ctx, ref.allocCString(options.local_ip),
    ref.allocCString(options.remote_ip), kHelloWorldPort, tx_flow.ref());
customCheck(ret === 0, 'machnet_connect()');

ret = machnet_shim.machnet_listen(
    channel_ctx, ref.allocCString(options.local_ip), kHelloWorldPort);
customCheck(ret === 0, 'machnet_listen()');

if (options.is_client) {
  // Client
  console.log('Running as client');

  let msgCounter = 0;
  const rx_flow = new MachnetFlow_t();
  const rx_buf = Buffer.alloc(1024);
  const msg = `Hello World!`;
  const msg_buffer = Buffer.from(msg, 'utf8');

  while (msgCounter < NUM_MESSAGES) {
    const startTime = process.hrtime();
    ret = machnet_shim.machnet_send(
        channel_ctx, tx_flow, msg_buffer, msg_buffer.length);

    if (ret === -1) {
      console.log(
          chalk.red(`Error: machnet_send() failed for message ${msgCounter}`));
      exit(1);
    } else {
      let bytesRead = 0;
      while (bytesRead === 0) {
        const result = machnet_shim.machnet_recv(
            channel_ctx, rx_buf, rx_buf.length, rx_flow.ref());
        if (result === -1) {
          console.log(chalk.red(
              `Error: machnet_recv() failed for message ${msgCounter}`));
          exit(1);
        }
        bytesRead = result;
      }

      const endTime = process.hrtime();
      const latency_us =
          (endTime[0] - startTime[0]) * 1e6 + (endTime[1] - startTime[1]) / 1e3;
      latencies_us.push(latency_us);
      msgCounter++;
    }

    if (msgCounter % (NUM_MESSAGES / 10) === 0) {
      console.log(`Sent ${msgCounter} messages of ${NUM_MESSAGES}`);
      print_stats(latencies_us);
      latencies_us.length = 0;
    }
  }


} else {
  // Server
  console.log('Running as server, waiting for messages from client');
  const buf = Buffer.alloc(1024);
  const rx_flow = new MachnetFlow_t();
  const replyMsg = `yes`;
  const replyBuffer = Buffer.from(replyMsg, 'utf8');

  while (true) {
    const bytesRead =
        machnet_shim.machnet_recv(channel_ctx, buf, buf.length, rx_flow.ref());

    if (bytesRead === -1) {
      console.log(chalk.red('Error: machnet_recv() failed'));
      continue;  // continue to poll for messages
    } else if (bytesRead > 0) {
      machnet_shim.machnet_send(
          channel_ctx, tx_flow, replyBuffer, replyBuffer.length);
    }
  }
}
