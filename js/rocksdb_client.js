const ref = require("ref-napi");
const dgram = require("dgram");
const commander = require("commander");
const chalk = require("chalk");
const { nsaas_shim, NSaaSNetFlow_t } = require("./nsaas_shim");

const kRocksDbServerPort = 888;

commander
  .option("-l, --local_ip <ip>", "Local IP address")
  .option("-r, --remote_ip <ip>", "Remote IP address")
  .option("-n, --num_keys <num>", "Number of keys at the server")
  .option("-o, --num_ops <num>", "Number of operations to perform")
  .option("-t, --transport <transport>", "Transport to use: nsaas/udp")
  .parse(process.argv);

const options = commander.opts();
if (
  !options.remote_ip ||
  !options.num_keys ||
  !options.num_ops ||
  !options.transport
) {
  console.log(
    chalk.red("Error: remote_ip, num_keys, num_ops, transport are required")
  );
  process.exit(1);
}

if (options.transport === "nsaas" && !options.local_ip) {
  console.log(chalk.red("Error: local_ip is required for nsaas transport"));
  process.exit(1);
}

console.log(options);

function customCheck(condition, message) {
  if (!condition) {
    console.log(chalk.red("Error: " + message));
    process.exit(1);
  } else {
    console.log(chalk.green("Success: " + message));
  }
}

function print_stats(arr) {
  const sorted = arr.sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)];
  const ninety = sorted[Math.floor(sorted.length * 0.9)];
  const ninety_nine = sorted[Math.floor(sorted.length * 0.99)];
  const ninety_nine_nine = sorted[Math.floor(sorted.length * 0.999)];

  console.log(`Median: ${Math.floor(median)} us, \
        90th: ${Math.floor(ninety)} us, \
        99th: ${Math.floor(ninety_nine)} us, \
        99.9th: ${Math.floor(ninety_nine_nine)} us`);
}

async function nsaasTransportClientAsync() {
  // Main logic
  var ret = nsaas_shim.nsaas_init();
  customCheck(ret === 0, "nsaas_init()");

  var channel_ctx = nsaas_shim.nsaas_attach();
  customCheck(channel_ctx !== null, "nsaas_attach()");

  const latencies_us = [];

  const tx_flow = new NSaaSNetFlow_t();
  var ret = nsaas_shim.nsaas_connect(
    channel_ctx,
    ref.allocCString(options.local_ip),
    ref.allocCString(options.remote_ip),
    kRocksDbServerPort,
    tx_flow.ref()
  );
  customCheck(ret === 0, "nsaas_connect()");

  ret = nsaas_shim.nsaas_listen(
    channel_ctx,
    ref.allocCString(options.local_ip),
    kRocksDbServerPort
  );
  customCheck(ret === 0, "nsaas_listen()");

  let msgCounter = 0;
  const rx_flow = new NSaaSNetFlow_t();
  const rx_buf = Buffer.alloc(1024);

  console.log(`Sending ${options.num_ops} queries`);

  async function nextRequest() {
    if (msgCounter >= options.num_ops) return;

    const startTime = process.hrtime();
    const keyIndex = Math.floor(Math.random() * options.num_keys);
    const key = "key" + keyIndex;
    const key_buffer = Buffer.from(key, "utf8");

    try {
      await new Promise((resolve, reject) => {
        ret = nsaas_shim.nsaas_send(
          channel_ctx,
          tx_flow,
          key_buffer,
          key_buffer.length
        );
        if (ret === -1) {
          reject(new Error(`Error: nsaas_send() failed for key ${key}`));
        } else {
          resolve();
        }
      });

      await new Promise((resolve, reject) => {
        let bytesRead = 0;
        while (bytesRead === 0) {
          const result = nsaas_shim.nsaas_recv(
            channel_ctx,
            rx_buf,
            rx_buf.length,
            rx_flow.ref()
          );
          if (result === -1) {
            reject(new Error(`Error: nsaas_recv() failed for key ${key}`));
          }
          bytesRead = result;
        }

        const endTime = process.hrtime();
        const latency_us =
          (endTime[0] - startTime[0]) * 1e6 + (endTime[1] - startTime[1]) / 1e3;
        latencies_us.push(latency_us);

        /*  
        const value = rx_buf.toString("utf8", 0, bytesRead);
        const expected_value = "value" + "x".repeat(200);
        if (value !== expected_value) {
          reject(
            new Error(`Error: Key '${key}' has an incorrect value`)
          );
        }
        */
        resolve();
      });
    } catch (err) {
      console.log(chalk.red(err.message));
      process.exit(1);
    }

    msgCounter++;

    if (msgCounter % (options.num_ops / 10) === 0) {
      console.log(`Sent ${msgCounter} RocksDB queries of ${options.num_ops}`);
      print_stats(latencies_us);
      latencies_us.length = 0;
    }

    nextRequest();
  }

  await nextRequest();
}

async function udpTransportClient() {
  const util = require("util");
  const socket = dgram.createSocket("udp4");
  var latencies_us = [];
  var startTime = process.hrtime();

  socket.on("error", (err) => {
    console.log(chalk.red(`Socket error:\n${err.stack}`));
    socket.close();
  });

  const sendRequest = util.promisify(socket.send).bind(socket);

  socket.on("message", (msg, rinfo) => {
    const endTime = process.hrtime();
    const latency_us =
      (endTime[0] - startTime[0]) * 1e6 + (endTime[1] - startTime[1]) / 1e3;
    latencies_us.push(latency_us);

    const value = msg.toString("utf8");
    const expected_value = "value" + "x".repeat(200);
    if (value !== expected_value) {
      console.log(chalk.red(`Error: Key '${key}' has an incorrect value`));
      console.log(`Expected value: ${expected_value}`);
      console.log(`Received value: ${value}`);
      process.exit(1);
    }

    nextRequest();
  });

  socket.bind(kRocksDbServerPort, options.local_ip, () => {
    console.log(
      `Socket is listening on ${options.local_ip}:${kRocksDbServerPort}`
    );
  });

  let msgCounter = 0;

  console.log(`Sending ${options.num_ops} queries`);

  async function nextRequest() {
    if (msgCounter >= options.num_ops) return;

    startTime = process.hrtime();
    const keyIndex = Math.floor(Math.random() * options.num_keys);
    const key = "key" + keyIndex;
    const key_buffer = Buffer.from(key, "utf8");

    try {
      await sendRequest(
        key_buffer,
        0,
        key_buffer.length,
        kRocksDbServerPort,
        options.remote_ip
      );
    } catch (err) {
      console.log(chalk.red(`Error: UDP send() failed for key ${key}`));
      exit(1);
    }

    msgCounter++;

    if (msgCounter % (options.num_ops / 10) === 0) {
      console.log(`Sent ${msgCounter} RocksDB queries of ${options.num_ops}`);
      print_stats(latencies_us);
      latencies_us.length = 0;
    }
  }

  nextRequest();
}

if (options.transport === "nsaas") {
  nsaasTransportClientAsync();
} else {
  udpTransportClient();
}
