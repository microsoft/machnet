// Copyright (C) 2023 Vahab Jabrayilov
// Email: vjabrayilov@cs.columbia.edu
//
// This file is part of the Machnet project.
//
// This project is licensed under the MIT License - see the LICENSE file for details

use std::path::PathBuf;
fn main() {
    let lib_path = PathBuf::from("../..");
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

    bindings
        .write_to_file(lib_path.join("bindings/rust/src/bindings.rs"))
        .expect("Couldn't write bindings!");
}
