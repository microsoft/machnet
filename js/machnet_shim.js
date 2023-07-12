const ffi = require('ffi-napi');
const ref = require('ref-napi');
const Struct = require('ref-struct-napi');

// Basic types and loading the lib
const NSaaSNetFlow_t = Struct({
  src_ip: 'uint32',
  dst_ip: 'uint32',
  src_port: 'uint16',
  dst_port: 'uint16'
});

const size_t = ref.types.size_t;
const voidPtr = ref.refType(ref.types.void);
const charPtr = ref.refType(ref.types.char);
const uint16 = ref.types.uint16;
const NSaaSNetFlowPtr = ref.refType(NSaaSNetFlow_t);

var dir = __dirname;
const libnsaas_shim_location = dir + '/libnsaas_shim.so';

console.log('Loading libnsaas_shim');
var nsaas_shim = ffi.Library(libnsaas_shim_location, {
  'nsaas_init': ['int', []],
  'nsaas_attach': ['pointer', []],
  'nsaas_listen': ['int', [voidPtr, charPtr, uint16]],
  'nsaas_connect': ['int', [voidPtr, charPtr, charPtr, uint16, NSaaSNetFlowPtr]],
  'nsaas_send': ['int', [voidPtr, NSaaSNetFlow_t, voidPtr, size_t]],
  'nsaas_recv': ['int', [voidPtr, voidPtr, size_t, NSaaSNetFlowPtr]]
});

module.exports = {
  nsaas_shim: nsaas_shim,
  NSaaSNetFlow_t: NSaaSNetFlow_t
};