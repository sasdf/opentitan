//! This script collects code coverage data for OpenTitan FPGA profiles.
//!
//! By taking advantage of Bazel C++ code coverage collection, this script is
//! able to be executed by the existing coverage collection mechanics.
//!
//! Bazel uses the lcov tool for gathering coverage data. There is also
//! an experimental support for clang llvm coverage, which uses the .profraw
//! data files to compute the coverage report.
//!
//! This script assumes the following environment variables are set:
//! - `COVERAGE_DIR``: Directory containing metadata files needed for coverage collection (e.g. gcda files, profraw).
//! - `COVERAGE_OUTPUT_FILE`: The coverage action output path.
//! - `ROOT`: Location from where the code coverage collection was invoked.
//! - `RUNFILES_DIR`: Location of the test's runfiles.
//! - `VERBOSE_COVERAGE`: Print debug info from the coverage scripts
//!
//! The script looks in $COVERAGE_DIR for the Rust metadata coverage files
//! (profraw) and uses lcov to get the coverage data. The coverage data
//! is placed in $COVERAGE_DIR as a `coverage.dat` file.

use anyhow::{anyhow, bail, Context, Result};
use object::{Object, ObjectSection, ObjectSymbol};
use std::collections::HashMap;
use std::env;
use std::ffi::OsStr;
use std::fs;
use std::io::{Read, Seek, Write};
use std::path::Path;
use std::path::PathBuf;
use std::process;
use zerocopy::AsBytes;

use coverage_lib::{
  process_elf,
  generate_view,
  debug_log,
  debug_environ,
};

fn main() -> Result<()> {
    debug_environ();

    let output_dir = PathBuf::from(env::var("TEST_UNDECLARED_OUTPUTS_DIR").unwrap());

    // Get the elf file to be tested.
    let elf = output_dir.join("test.elf");
    debug_log!("elf: {elf:?}");

    // Index elf profile data with build id.
    match process_elf(&elf) {
        Ok(elf) => {
            debug_log!("Loaded {:?} = {}", elf.file_name, elf.build_id);
            generate_view(&elf)?;
        }
        Err(err) => eprintln!("Failed to parse {elf:?} for coverage view: {err:?}"),
    };

    debug_log!("Success!");
    Ok(())
}
