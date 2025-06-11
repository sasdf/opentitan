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
  search_by_extension,
  debug_log,
  process_elf,
  process_profraw,
  llvm_cov_export,
  llvm_profdata_merge,
  get_runfiles_dir,
  debug_environ,
};

fn main() -> Result<()> {
    debug_environ();

    let coverage_dir = PathBuf::from(env::var("COVERAGE_DIR").unwrap());
    let runfiles_dir = get_runfiles_dir();

    let profraw_files = search_by_extension(&coverage_dir, "profraw");
    debug_log!("profraw_files: {:?}", profraw_files);

    // Collect all elf files in the runfiles.
    let runfiles_manifest = runfiles_dir.join("MANIFEST");
    let elf_files: Vec<PathBuf> = fs::read_to_string(&runfiles_manifest)
        .unwrap()
        .lines()
        .filter_map(|path| {
            let pair = path
                .split_once(' ')
                .expect("manifest file contained unexpected content");
            if pair.0.ends_with(".elf") {
                Some(PathBuf::from(pair.1))
            } else {
                None
            }
        })
        .collect();
    debug_log!("elf_files: {:?}", elf_files);

    // Index elf profile data with build id.
    let mut profile_map = HashMap::new();
    for path in &elf_files {
        match process_elf(path) {
            Ok(elf) => {
                debug_log!("Loaded {:?} = {}", elf.file_name, elf.build_id);
                profile_map.insert(elf.build_id.clone(), elf);
            }
            Err(err) => eprintln!("Skip {path:?}: {err:?}"),
        }
    }

    // Correlate profile data with counters from the device.
    for path in &profraw_files {
        let profile = process_profraw(path, &profile_map).unwrap();
        // We use .xprofdata instead of .profdata to avoid lcov_merger from parsing it.
        let profdata_file = path.with_extension("xprofdata");
        let lcov_file = path.with_extension("dat");
        llvm_profdata_merge(&path, &profdata_file);
        llvm_cov_export("lcov", &profdata_file, &profile.objects, &lcov_file);
    }

    debug_log!("Success!");
    Ok(())
}
