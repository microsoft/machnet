# TODO

<!-- - [ ] Fix warning "128-bit integers don't currently have a known stable ABI". -->
  <!-- - Potential cause: `stdlib.h` and other headers are included in `bindgen`'s output. -->
- [ ] Add tests to `src/lib.r` to ensure that the bindings are working correctly.
- [ ] `cargo test --doc` failing if `libmachnet_shim.so` is not in `LD_LIBRARY_PATH`.
- [ ] Populate [README](README.md) with more information.
