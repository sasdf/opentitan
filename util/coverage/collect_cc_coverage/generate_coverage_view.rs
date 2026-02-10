// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

//! This tool creates a code coverage view for OpenTitan firmware.
//!
//! A "coverage view" represents the total set of executable lines and functions
//! present in a given ELF, as determined by DWARF debug information and LLVM
//! instrumentation metadata. This is used as a filter to generate coverage
//! reports of specific firmware by hiding code discarded by the compiler/linker.
//!
//! This tool assumes the following environment variables are set:
//! - `RUNFILES_DIR`: Location of the test's runfiles.
//! - `VERBOSE_COVERAGE`: Print debug info from the coverage scripts.

use anyhow::{anyhow, Context, Result};
use clap::Parser;
use object::{Object, ObjectSection};
use serde::Deserialize;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

use coverage_lib::{debug_environ, llvm_cov_export, llvm_profdata_merge, ProfileData};

/// Source files that should not correlate with DWARF line info.
///
/// For some source files, such as those containing macro-defined functions,
/// DWARF debug line information point to the macro invocation site,
/// whereas the LLVM coverage mapping points to regions within the macro
/// body. These files are treated as exceptions to DWARF-based filtering.
const IGNORE_DWARF: &[&str] = &["sw/device/silicon_creator/lib/manifest.h"];

#[derive(Debug, Parser)]
#[command(
    name = "generate-coverage-view",
    about = "Creates code coverage view for OpenTitan firmware"
)]
struct Opts {
    /// Path to the ELF file to be analyzed.
    #[arg(long)]
    elf: PathBuf,

    /// Directory where the coverage report is stored.
    #[arg(long)]
    output_dir: PathBuf,

    /// ELF kind, choices only from {'ibex', 'otbn'}.
    #[arg(long, default_value = "ibex", value_parser = ["ibex", "otbn"])]
    kind: String,
}

/// Root structure for LLVM's coverage export JSON format.
#[derive(Debug, Deserialize)]
struct ExportJson {
    data: Vec<ExportData>,
}

/// Contains coverage information for a set of functions.
#[derive(Debug, Deserialize)]
struct ExportData {
    functions: Vec<Function>,
}

/// Represents an individual function's coverage metadata.
#[derive(Debug, Deserialize)]
struct Function {
    name: String,
    filenames: Vec<String>,
    regions: Vec<Region>,
}

/// Represents a source code region from LLVM export JSON.
#[derive(Debug, Deserialize)]
struct Region {
    line_start: u64,
    #[allow(dead_code)]
    col_start: u64,
    line_end: u64,
    #[allow(dead_code)]
    col_end: u64,
    execution_count: u64,
    file_id: usize,
    #[allow(dead_code)]
    expanded_file_id: usize,
    kind: u64,
}

/// Aggregated executable components for a specific source file.
#[derive(Debug, Default)]
struct ExecutableRegion {
    /// Sorted set of line numbers that were actually executable in the binary.
    lines: BTreeSet<u64>,
    /// Sorted map of function names to their starting line numbers.
    functions: BTreeMap<String, u64>,
}

/// Extracts executable line information from the ELF using DWARF debug info.
///
/// Maps source file paths to a set of line numbers that correspond to machine instructions
/// in the `.text` section of the provided ELF.
fn get_dwarf_line_map(elf_path: &Path) -> Result<BTreeMap<String, BTreeSet<u64>>> {
    let mut line_map: BTreeMap<String, BTreeSet<u64>> = BTreeMap::new();
    let dwarf = addr2line::Loader::new(elf_path).unwrap();

    let obj_file = fs::read(elf_path)?;
    let obj = object::File::parse(&*obj_file)?;

    for section in obj
        .sections()
        .filter(|s| s.kind() == object::SectionKind::Text)
    {
        let start = section.address();
        let end = start + section.size();
        if let Ok(ranges) = dwarf.find_location_range(start, end) {
            for (_addr, _size, loc) in ranges {
                if let (Some(file), Some(line)) = (loc.file, loc.line) {
                    let file = file
                        .replace("/proc/self/cwd/./", "")
                        .replace("/proc/self/cwd/", "");
                    line_map.entry(file).or_default().insert(line as u64);
                }
            }
        }
    }
    Ok(line_map)
}

/// Identifies source regions and functions actually present in the final binary.
///
/// Cross-references LLVM coverage metadata with the dwarf line map to filter out
/// functions or regions that might have been removed by the compiler/linker.
fn find_executable_regions(
    functions: &[Function],
    line_map: &BTreeMap<String, BTreeSet<u64>>,
) -> Result<BTreeMap<String, ExecutableRegion>> {
    let mut executable_regions: BTreeMap<String, ExecutableRegion> = BTreeMap::new();

    for func in functions {
        let body = &func.regions[0];
        // Ensure the first region is the main function body.
        assert_eq!(body.kind, 0);
        assert_eq!(body.file_id, 0);

        // If the function is not reachable, all expansions should be unreachable too.
        if body.execution_count == 0 {
            continue;
        }

        let body_filename = &func.filenames[body.file_id];
        let func_name = func.name.split(':').last().unwrap_or(&func.name);

        // Use the DWARF line map to determine if any machine instructions were
        // generated for this function. This allows us to exclude functions
        // that were stripped by the compiler or linker.
        let func_hit = line_map
            .get(&func.filenames[0])
            .map(|lines_hit| {
                lines_hit
                    .range(body.line_start..=body.line_end)
                    .next()
                    .is_some()
            })
            .unwrap_or(false);

        if !func_hit {
            continue;
        }

        let body_entry = executable_regions.entry(body_filename.clone()).or_default();
        body_entry
            .functions
            .insert(func_name.to_string(), body.line_start);

        for region in func
            .regions
            .iter()
            .filter(|r| r.kind == 0 && r.execution_count != 0)
        {
            assert_eq!(region.expanded_file_id, 0);
            let filename = &func.filenames[region.file_id];
            let entry = executable_regions.entry(filename.clone()).or_default();
            entry.lines.extend(region.line_start..=region.line_end);
        }
    }

    // Cross-reference DWARF line info with collected regions to ensure every
    // executable instruction is mapped to a coverage region.
    for (filename, lines) in line_map {
        if IGNORE_DWARF.contains(&filename.as_str()) {
            continue;
        }
        if let Some(executable) = executable_regions.get(filename) {
            for line in lines {
                if !executable.lines.contains(line) {
                    return Err(anyhow!(
                        "line {line} in {filename} has DWARF info but no coverage regions"
                    ));
                }
            }
        }
    }

    Ok(executable_regions)
}

/// Filters raw LCOV output to only include regions that exist in the binary.
///
/// This creates the "view" by taking the LCOV input and removing
/// functions that has no related dwarf line info.
fn filter_lcov_view(
    lcov_input: &str,
    executable_regions: &BTreeMap<String, ExecutableRegion>,
) -> Result<String> {
    let mut filtered_lcov = String::new();

    // Iterate through each source file record in the input LCOV data.
    for record in lcov_input
        .split("\nend_of_record\n")
        .filter(|r| !r.trim().is_empty())
    {
        let mut lines = record.lines();
        let sf_line = lines.next().context("Missing SF line")?;
        let filename = sf_line.strip_prefix("SF:").context("Invalid SF line")?;

        let is_asm = filename.to_lowercase().ends_with(".s");
        let is_ignore_dwarf = IGNORE_DWARF.contains(&filename);

        if is_asm || is_ignore_dwarf {
            filtered_lcov.push_str(record);
            filtered_lcov.push_str("\nend_of_record\n");
            continue;
        }

        if let Some(executable) = executable_regions.get(filename) {
            filtered_lcov.push_str(sf_line);
            filtered_lcov.push('\n');

            // Emit function definitions (FN) and execution counts (FNDA).
            for (func_name, line) in &executable.functions {
                filtered_lcov.push_str(&format!("FN:{line},{func_name}\n"));
            }
            for func_name in executable.functions.keys() {
                filtered_lcov.push_str(&format!("FNDA:1,{func_name}\n"));
            }
            // Only keep DA lines that represent executable instructions.
            for line in lines {
                if let Some(da_content) = line.strip_prefix("DA:") {
                    if let Some((lineno_str, _)) = da_content.split_once(',') {
                        if let Ok(lineno) = lineno_str.parse::<u64>() {
                            if executable.lines.contains(&lineno) {
                                filtered_lcov.push_str(&format!("DA:{lineno},1\n"));
                            }
                        }
                    }
                }
            }
            filtered_lcov.push_str("end_of_record\n");
        }
    }
    Ok(filtered_lcov)
}

fn handle_ibex(opts: &Opts) -> Result<()> {
    let lcov_input_file = opts.output_dir.join("coverage.org.dat");
    let lcov_output_file = opts.output_dir.join("coverage.dat");
    let json_output_file = opts.output_dir.join("coverage.json");
    let profdata_file = opts.output_dir.join("coverage.profdata");
    let profraw_file = opts.output_dir.join("coverage.profraw");

    // Generate a profraw file where all counters are marked as "hit".
    let profile = ProfileData::from_elf(&opts.elf)?;
    println!("Loaded {:?} = {}", profile.file_name, profile.build_id);
    profile.generate_view_profraw(&profraw_file)?;

    // Export to JSON and LCOV formats for analysis.
    llvm_profdata_merge(&profraw_file, &profdata_file);
    llvm_cov_export("text", &profdata_file, &profile.elf, &json_output_file);
    llvm_cov_export("lcov", &profdata_file, &profile.elf, &lcov_input_file);

    // Determine which source lines actually executable in the binary.
    let json_str = fs::read_to_string(&json_output_file)?;
    let export: ExportJson = serde_json::from_str(&json_str)?;
    let functions = &export.data.first().context("Empty export data")?.functions;
    let line_map = get_dwarf_line_map(&opts.elf)?;
    let executable_regions = find_executable_regions(functions, &line_map)?;

    // Filter the initial LCOV to match only the executable regions.
    let lcov_str = fs::read_to_string(&lcov_input_file)?;
    let filtered_lcov = filter_lcov_view(&lcov_str, &executable_regions)?;

    // Write the filtered LCOV.
    fs::write(&lcov_output_file, filtered_lcov)?;
    Ok(())
}

fn handle_otbn(opts: &Opts) -> Result<()> {
    let lcov_output_file = opts.output_dir.join("coverage.dat");
    let line_map = get_dwarf_line_map(&opts.elf)?;
    // OTBN binaries lack LLVM coverage metadata; we generate the coverage view
    // from DWARF line information only.
    let mut lcov = String::new();
    for (filename, lines) in line_map {
        lcov.push_str(&format!("SF:{}\n", filename));
        for line in lines {
            lcov.push_str(&format!("DA:{},1\n", line));
        }
        lcov.push_str("end_of_record\n");
    }
    fs::write(&lcov_output_file, lcov)?;
    Ok(())
}

fn main() -> Result<()> {
    debug_environ();
    let opts = Opts::parse();

    fs::create_dir_all(&opts.output_dir)?;

    match opts.kind.as_str() {
        "ibex" => handle_ibex(&opts)?,
        "otbn" => handle_otbn(&opts)?,
        _ => unreachable!("Value parser should restrict kind to ibex or otbn"),
    }

    println!("Success!");
    Ok(())
}
