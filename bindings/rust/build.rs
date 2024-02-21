// Copyright (C) 2023 Vahab Jabrayilov
// Email: vjabrayilov@cs.columbia.edu
//
// This file is part of the Machnet project.
//
// This project is licensed under the MIT License - see the LICENSE file for details

use std::{env, path::PathBuf};
fn main() {
    let lib_path = "resources";

    println!("cargo:rustc-link-search=native={}", lib_path);
    println!("cargo:rustc-link-lib=machnet_shim");

    let bindings = bindgen::Builder::default()
        .header(format!("{}/machnet.h", lib_path))
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .allowlist_function(".*machnet.*")
        .generate()
        .expect("Unable to generate bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}
