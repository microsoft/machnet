use std::{env, path::PathBuf};
fn main() {
    let lib_path = "../..";

    println!("cargo:rustc-link-search=native={}", lib_path);
    println!("cargo:rustc-link-lib=machnet_shim");
    // construct a string

    let bindings = bindgen::Builder::default()
        .header(format!("{}/src/ext/machnet.h", lib_path))
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .allowlist_function(".*machnet.*")
        .generate()
        .expect("Unable to generate bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}
