const ffi = require('ffi-napi');
const ref = require('ref-napi');
const Struct = require('ref-struct-napi');

// Basic types and loading the lib
const MachnetFlow_t = Struct({
  src_ip: 'uint32',
  dst_ip: 'uint32',
  src_port: 'uint16',
  dst_port: 'uint16'
});

const size_t = ref.types.size_t;
const voidPtr = ref.refType(ref.types.void);
const charPtr = ref.refType(ref.types.char);
const uint16 = ref.types.uint16;
const MachnetFlowPtr = ref.refType(MachnetFlow_t);

var dir = __dirname;
const libmachnet_shim_location = dir + '/libmachnet_shim.so';

console.log('Loading libmachnet_shim');
var machnet_shim = ffi.Library(libmachnet_shim_location, {
  'machnet_init': ['int', []],
  'machnet_attach': ['pointer', []],
  'machnet_listen': ['int', [voidPtr, charPtr, uint16]],
  'machnet_connect': ['int', [voidPtr, charPtr, charPtr, uint16, MachnetFlowPtr]],
  'machnet_send': ['int', [voidPtr, MachnetFlow_t, voidPtr, size_t]],
  'machnet_recv': ['int', [voidPtr, voidPtr, size_t, MachnetFlowPtr]]
});

module.exports = {
  machnet_shim: machnet_shim,
  MachnetFlow_t: MachnetFlow_t
};
