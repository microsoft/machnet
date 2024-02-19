use std::{env, path::PathBuf};
fn main() {
    // Assumes MACHNET env variable set to the path of Machnet root directory
    // in this case $HOME/machnet
    let lib_path = PathBuf::from(env::var("MACHNET").unwrap());
    // let lib_path = "../..";

    println!(
        "cargo:rustc-link-search=native={}",
        lib_path.to_str().unwrap()
    );
    println!("cargo:rustc-link-lib=machnet_shim");

    let bindings = bindgen::Builder::default()
        .header(format!("{}/src/ext/machnet.h", lib_path.to_str().unwrap()))
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .allowlist_function(".*machnet.*")
        .generate()
        .expect("Unable to generate bindings");

    // let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(lib_path.join("bindings/rust/src/bindings.rs"))
        .expect("Couldn't write bindings!");
}
