const { performance } = require('perf_hooks');
g_str = 'a'.repeat(32);
g_bytes = Buffer.from(g_str, 'utf8');

// Function to benchmark execution time
function benchmark(func, iterations) {
  const start = process.hrtime.bigint();
  for (let i = 0; i < iterations; i++) {
    func();
  }
  const end = process.hrtime.bigint();
  const duration = end - start;
  console.log(`${func.name} took ${duration / BigInt(iterations)} nanoseconds per iteration.`);
}

// Functions to be benchmarked
function now() {
  return Date.now();
}

function hrtime() {
  return process.hrtime();
}

function bufferAlloc() {
  return Buffer.alloc(1024);
}

function bufferToString() {
  return g_bytes.toString('utf8');
}

function bufferFromString() {
  return Buffer.from(g_str, 'utf8');
}

function consoleLog() {
  console.log('This output will be hidden.');
}

// Run benchmarks
var iterations = 100000;
benchmark(now, iterations);
benchmark(bufferAlloc, iterations);
benchmark(bufferToString, iterations);
benchmark(bufferFromString, iterations);
benchmark(hrtime, iterations);

console.log('Waiting 5 seconds before running next set of benchmarks...');

var iterations = 1000;
setTimeout(() => { benchmark(consoleLog, iterations); }, 5000);
