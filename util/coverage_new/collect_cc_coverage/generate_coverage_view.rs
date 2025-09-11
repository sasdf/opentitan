// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

//! This tool creates code coverage view for OpenTitan firmware.
//!
//! This tool assumes the following environment variables are set:
//! - `ROOT`: Location from where the code coverage collection was invoked.
//! - `RUNFILES_DIR`: Location of the test's runfiles.
//! - `VERBOSE_COVERAGE`: Print debug info from the coverage scripts
//! - `COVERAGE_DIR`: The directory where coverage artifacts are stored.
//! - `TEST_UNDECLARED_OUTPUTS_DIR`: The directory where extra coverage report is stored.

use anyhow::Result;
use std::env;
use std::fs;

use coverage_lib::{
    debug_environ, debug_log, llvm_cov_export, llvm_profdata_merge, path_from_env, ProfileData,
};

fn generate_view(profile: &ProfileData) -> Result<()> {
    let coverage_dir = path_from_env("COVERAGE_DIR");
    let output_dir = path_from_env("TEST_UNDECLARED_OUTPUTS_DIR");
    let lcov_output_file = output_dir.join("coverage.dat");
    let json_output_file = output_dir.join("coverage.json");
    let profdata_file = output_dir.join("coverage.profdata");
    let profraw_file = coverage_dir.join("coverage.profraw");
    let bazel_output_file = coverage_dir.join("coverage.dat");
    profile.generate_view_profraw(&profraw_file)?;
    llvm_profdata_merge(&profraw_file, &profdata_file);
    llvm_cov_export("lcov", &profdata_file, &profile.elf, &lcov_output_file);
    llvm_cov_export("text", &profdata_file, &profile.elf, &json_output_file);
    fs::copy(&lcov_output_file, &bazel_output_file)?;
    Ok(())
}

fn main() -> Result<()> {
    debug_environ();

    let output_dir = path_from_env("TEST_UNDECLARED_OUTPUTS_DIR");

    // Get the elf file to be tested.
    let elf = output_dir.join("test.elf");
    debug_log!("elf: {elf:?}");

    // Index elf profile data with build id.
    match ProfileData::from_elf(&elf) {
        Ok(profile) => {
            debug_log!("Loaded {:?} = {}", profile.file_name, profile.build_id);
            generate_view(&profile)?;
        }
        Err(err) => eprintln!("Failed to parse {elf:?} for coverage view: {err:?}"),
    };

    debug_log!("Success!");
    Ok(())
}
