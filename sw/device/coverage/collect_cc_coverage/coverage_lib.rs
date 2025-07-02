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
use byteorder::{LittleEndian, ReadBytesExt};
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

use std::fs::File;
use tar::Archive;


pub const BUILD_ID_SIZE: usize = 20;
pub const PRF_MAGIC: u64 = 0xff6c70726f665281;
pub const OTC_MAGIC: u64 = 0x7265766f43544f81; // File magic: \x81OTCover
pub const PRF_VERSION: u64 = 8;
pub const PRF_DATA_ENTRY_SIZE: u64 = 40;
pub const VARIANT_MASK_BYTE_COVERAGE: u64 = (0x1 << 60);

pub const ASM_COUNTER_FILE: &str = "SF:sw/device/coverage/asm_counters.c";
pub const ASM_COUNTER_SIZE: usize = 96;


#[macro_export]
macro_rules! debug_log {
    ($($arg:tt)*) => {
        if env::var("VERBOSE_COVERAGE").is_ok() {
            eprintln!($($arg)*);
        }
    };
}

pub fn get_runfiles_dir () -> PathBuf {
    let execroot = PathBuf::from(env::var("ROOT").unwrap());
    let mut runfiles_dir = PathBuf::from(env::var("RUNFILES_DIR").unwrap());

    if !runfiles_dir.is_absolute() {
        runfiles_dir = execroot.join(runfiles_dir);
    }

    debug_log!("ROOT: {}", execroot.display());
    debug_log!("RUNFILES_DIR: {}", runfiles_dir.display());

    return runfiles_dir
}

pub fn debug_environ() {
    debug_log!("Environment variables::");
    for (key, value) in env::vars() {
        debug_log!("{}={}", key, value);
    }
}

pub fn search_by_extension(dir: &PathBuf, extension: &str) -> Vec<PathBuf> {
    fs::read_dir(dir)
        .unwrap()
        .flatten()
        .filter_map(|entry| {
            let path = entry.path();
            if let Some(ext) = path.extension() {
                if ext == extension {
                    return Some(path);
                }
            }
            None
        })
        .collect()
}

pub fn recursive_search_by_extension(dir: &PathBuf, extension: &str) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    if let Ok(entries) = fs::read_dir(dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                paths.extend(recursive_search_by_extension(&path, extension));
            } else if let Some(ext) = path.extension() {
                if ext == extension {
                    paths.push(path);
                }
            }
        }
    }
    paths
}

#[derive(AsBytes, Debug, Default)]
#[repr(C)]
pub struct ProfileHeader {
    pub Magic: u64,
    pub Version: u64,
    pub BinaryIdsSize: u64,
    pub NumData: u64,
    pub PaddingBytesBeforeCounters: u64,
    pub NumCounters: u64,
    pub PaddingBytesAfterCounters: u64,
    pub NamesSize: u64,
    pub CountersDelta: u64,
    pub NamesDelta: u64,
    pub ValueKindLast: u64,
}

pub struct ProfileCounter {
    pub build_id: String,
    pub cnts: Vec<u8>,
}

pub struct ProfileData {
    pub build_id: String,
    pub elf: PathBuf,
    pub objects: Vec<String>,
    pub file_name: String,
    pub header: ProfileHeader,
    pub cnts_size: u64,
    pub data: Vec<u8>,
    pub names: Vec<u8>,
}

pub fn process_elf(path: &PathBuf) -> Result<ProfileData> {
    let elf = fs::read(path).context("failed to read ELF")?;
    let elf = object::File::parse(&*elf).context("failed to parse ELF")?;
    let file_name = path.file_name().context("Missing filename")?;
    let file_name = file_name.to_str().context("Missing filename")?.to_string();

    let prf_cnts = elf
        .section_by_name("__llvm_prf_cnts")
        .context("__llvm_prf_cnts not found")?;
    let prf_data = elf
        .section_by_name("__llvm_prf_data")
        .context("__llvm_prf_data not found")?;
    let prf_names = elf
        .section_by_name("__llvm_prf_names")
        .context("__llvm_prf_names not found")?;
    let build_id = elf
        .section_by_name(".note.gnu.build-id")
        .context(".note.gnu.build-id not found")?;

    let build_id = build_id.data()?;
    let build_id = &build_id[build_id.len() - BUILD_ID_SIZE..];
    let build_id = hex::encode(build_id);
    debug_log!("Got build_id = {build_id:?}");

    if prf_data.size() % PRF_DATA_ENTRY_SIZE != 0 {
        bail!("Invalid __llvm_prf_data section size");
    }

    Ok(ProfileData {
        build_id: build_id,
        elf: path.clone(),
        objects: load_object_list(&path.with_extension("objs.tar"))?,
        file_name,
        header: ProfileHeader {
            Magic: PRF_MAGIC,
            Version: 0, // The field will be set later.
            BinaryIdsSize: 0,
            NumData: prf_data.size() / PRF_DATA_ENTRY_SIZE,
            PaddingBytesBeforeCounters: 0,
            NumCounters: 0, // The field will be set later.
            PaddingBytesAfterCounters: 0,
            NamesSize: prf_names.size(),
            CountersDelta: prf_cnts.address().wrapping_sub(prf_data.address()) as u32 as u64,
            NamesDelta: prf_names.address() as u32 as u64,
            ValueKindLast: 1,
        },
        cnts_size: prf_cnts.size(),
        data: prf_data.data()?.to_vec(),
        names: prf_names.data()?.to_vec(),
    })
}

pub fn decompress(path: &PathBuf) -> Result<ProfileCounter> {
    let mut f = std::fs::File::open(path)?;

    // Check header
    let magic_bytes = f.read_u64::<LittleEndian>()?;
    if magic_bytes != OTC_MAGIC {
        bail!("Unknown profraw file magic bytes.");
    }

    // Read build id
    let mut build_id = [0u8; BUILD_ID_SIZE];
    f.read_exact(&mut build_id)?;

    // Decompressed cnts
    let mut cnts: Vec<u8> = Vec::new();

    let mut byte = [0u8; 1];
    while f.read_exact(&mut byte).is_ok() {
        if byte[0] == 0 || byte[0] == 0xff {
            let tag = byte[0];
            // Compressed padding.
            f.read_exact(&mut byte)?; // Read the padding marker/size.

            // Determine the padding length.
            let pad = match byte[0] {
                0xFE => {
                    let mut pad = [0u8; 2];
                    f.read_exact(&mut pad)?;
                    u16::from_le_bytes(pad) as usize
                }
                0xFF => {
                    let mut pad = [0u8; 4];
                    f.read_exact(&mut pad[..3])?;
                    u32::from_le_bytes(pad) as usize
                }
                // Any other value is the padding length itself.
                _ => byte[0] as usize,
            };
            let new_size = cnts.len() + pad;
            // Prevent excessive counter than what can be held on OpenTitan.
            if new_size > 1024 * 1024 {
                bail!("Decompressed counter is too large");
            }
            cnts.resize(new_size, tag);
        } else {
            // Packed data byte.
            for k in 0..8 {
                let bit = (byte[0] >> k) & 1;
                // If bit is 0, original value is 0xff. Otherwise 0x00.
                cnts.push(if bit == 0 { 0xff } else { 0x00 });
            }
        }
    }

    Ok(ProfileCounter {
        build_id: hex::encode(build_id),
        cnts: cnts.to_vec(),
    })
}

pub fn process_counter<'a>(path: &PathBuf, counter: &ProfileCounter, output: &PathBuf,
                           profile_map: &'a HashMap<String, ProfileData>) -> Result<&'a ProfileData> {
    let ProfileCounter { build_id, cnts } = counter;

    // Counters only, try to correlate with elf data.
    let profile = match profile_map.get(build_id) {
        Some(profile) => profile,
        None => {
            eprintln!("ERROR: Missing profile with build-id {build_id:?}.");
            eprintln!("Loaded elf profiles:");
            for (bid, profile) in profile_map {
                eprintln!("  {bid} : {:?}", profile.elf);
            }
            bail!("Missing profile with build-id {build_id:?}.");
        }
    };
    eprintln!("Profile:");
    eprintln!("  Profraw:  {:?}", path);
    eprintln!("  BuildID:  {}", build_id);
    eprintln!("  Firmware: {:?}", profile.file_name);
    debug_log!("{:?}", profile.elf);

    if profile.cnts_size != cnts.len() as u64 {
        bail!("cnts size mismatched");
    }

    let header = ProfileHeader{
        Version: PRF_VERSION | VARIANT_MASK_BYTE_COVERAGE,
        NumCounters: cnts.len() as u64,
        ..profile.header
    };
    debug_log!("{:#?}", header);
    assert_eq!(profile.data.len() as u64, header.NumData * PRF_DATA_ENTRY_SIZE);
    assert_eq!(profile.names.len() as u64, header.NamesSize);

    let mut f = std::fs::File::create(output)?;
    f.write_all(header.as_bytes())?;
    f.write_all(&profile.data)?;
    f.write_all(&cnts)?;
    f.write_all(&profile.names)?;

    let size = f.seek(std::io::SeekFrom::Current(0))?;
    if size % 8 != 0 {
        let buf = [0; 8];
        let pad: usize = (8 - (size % 8)) as usize;
        f.write_all(&buf[..pad])?;
    }

    Ok(profile)
}

pub fn generate_view_profraw(profile: &ProfileData, output_path: &PathBuf) -> Result<()> {
    let cnts = vec![0x00; profile.cnts_size as usize];

    let header = ProfileHeader{
        Version: PRF_VERSION | VARIANT_MASK_BYTE_COVERAGE,
        NumCounters: cnts.len() as u64,
        ..profile.header
    };
    debug_log!("{:#?}", header);
    assert_eq!(profile.data.len() as u64, header.NumData * PRF_DATA_ENTRY_SIZE);
    assert_eq!(profile.names.len() as u64, header.NamesSize);

    let mut f = std::fs::File::create(output_path)?;
    f.write_all(header.as_bytes())?;
    f.write_all(&profile.data)?;
    f.write_all(&cnts)?;
    f.write_all(&profile.names)?;

    let size = f.seek(std::io::SeekFrom::Current(0))?;
    if size % 8 != 0 {
        let buf = [0; 8];
        let pad: usize = (8 - (size % 8)) as usize;
        f.write_all(&buf[..pad])?;
    }

    Ok(())
}

pub fn llvm_profdata_merge(profraw_file: &PathBuf, profdata_file: &PathBuf) {
    let llvm_profdata = &env::var("LLVM_PROFDATA").unwrap();
    debug_log!("llvm_profdata: {llvm_profdata}");

    // "${LLVM_PROFDATA}" merge -output "${profdata_file}" "${profraw_file}"
    let mut llvm_profdata_cmd = process::Command::new(llvm_profdata);
    llvm_profdata_cmd
        .arg("merge")
        .arg("--sparse")
        .arg(profraw_file)
        .arg("--output")
        .arg(profdata_file);

    debug_log!("Spawning {:#?}", llvm_profdata_cmd);
    let status = llvm_profdata_cmd
        .status()
        .expect("Failed to spawn llvm-profdata process");

    if !status.success() {
        process::exit(status.code().unwrap_or(1));
    }
}

pub fn llvm_cov_export(format: &str, profdata_file: &PathBuf, objects: &Vec<String>, output_file: &PathBuf) {
    let execroot = PathBuf::from(env::var("ROOT").unwrap());
    let llvm_cov = &env::var("LLVM_COV").unwrap();
    debug_log!("llvm_cov: {llvm_cov}");

    // "${LLVM_COV}" export -instr-profile "${profdata_file" -format=lcov \
    //     -ignore-filename-regex='^/tmp/.+' \
    //     ${objects} | sed 's#/proc/self/cwd/##' > "${output_file}"
    let mut llvm_cov_cmd = process::Command::new(llvm_cov);
    llvm_cov_cmd
        .arg("export")
        .arg(format!("-format={format}"))
        .arg("-instr-profile")
        .arg(profdata_file)
        .arg("-ignore-filename-regex='.*external/.+'")
        .arg("-ignore-filename-regex='/tmp/.+'")
        .arg(format!("-path-equivalence=.,'{}'", execroot.display()))
        .args(objects)
        .stdout(process::Stdio::piped());

    debug_log!("Spawning {:#?}", llvm_cov_cmd);
    let child = llvm_cov_cmd
        .spawn()
        .expect("Failed to spawn llvm-cov process");

    let output = child.wait_with_output().expect("llvm-cov process failed");

    // Parse the child process's stdout to a string now that it's complete.
    debug_log!("Parsing llvm-cov output");
    let report_str = std::str::from_utf8(&output.stdout).expect("Failed to parse llvm-cov output");

    debug_log!("Writing output to {}", output_file.display());
    fs::write(
        output_file,
        report_str
            .replace("/proc/self/cwd/", "")
            .replace(&execroot.display().to_string(), ""),
    )
    .unwrap();
}

pub fn generate_view(profile: &ProfileData) -> Result<()> {
    let coverage_dir = PathBuf::from(env::var("COVERAGE_DIR").unwrap());
    let output_dir = PathBuf::from(env::var("TEST_UNDECLARED_OUTPUTS_DIR").unwrap());
    let lcov_output_file = output_dir.join("coverage.dat");
    let json_output_file = output_dir.join("coverage.json");
    let profdata_file = output_dir.join("coverage.profdata");
    let profraw_file = coverage_dir.join("coverage.profraw");
    generate_view_profraw(&profile, &profraw_file)?;
    llvm_profdata_merge(&profraw_file, &profdata_file);
    llvm_cov_export("lcov", &profdata_file, &profile.objects, &lcov_output_file);
    append_asm_view(&lcov_output_file)?;
    llvm_cov_export("text", &profdata_file, &profile.objects, &json_output_file);
    Ok(())
}

pub fn load_object_list(path: &PathBuf) -> Result<Vec<String>> {
    let tmp_dir = PathBuf::from(env::var("TEST_TMPDIR").unwrap());

    // create a random temp dir under `tmp_dir`
    let tmp_dir = tmp_dir.join(format!("objs-{}", rand::random::<u64>()));
    fs::create_dir_all(&tmp_dir).context("failed to create temporary directory")?;

    debug_log!("Extracting archive {} to {}", path.display(), tmp_dir.display());

    let mut ar = Archive::new(File::open(path)?);

    // unpack to $i.o for each i, e in enumerate(ar.entries)
    let mut results: Vec<String> = Vec::new();
    for (i, entry) in ar.entries()?.enumerate() {
        let mut entry = entry?;
        let path = tmp_dir.join(format!("{}.o", i));
        let path = path.to_str().unwrap().to_owned();
        entry.unpack(&path)?;
        results.push("-object".to_string());
        results.push(path);
    }

    return Ok(results);
}

pub fn append_asm_coverage(counter: &ProfileCounter, output_path: &PathBuf) -> Result<()> {
    if counter.cnts.len() < ASM_COUNTER_SIZE {
        bail!("Manual coverage counter is too short.");
    }

    let (cnts, _) = counter.cnts.split_at(ASM_COUNTER_SIZE);

    let mut f = std::fs::OpenOptions::new()
        .append(true)
        .open(output_path)?;

    writeln!(f, "{ASM_COUNTER_FILE}")?;

    for (i, byte) in cnts.iter().enumerate() {
        let count = match byte {
            0xff => 0, // Not executed
            0x00 => 1, // Executed
            _ => bail!("Invalid asm coverage counter value: {byte}"),
        };
        writeln!(f, "DA:{},{}", i + 1, count)?;
    }

    writeln!(f, "end_of_record")?;

    Ok(())
}

pub fn append_asm_view(output_path: &PathBuf) -> Result<()> {
    let mut f = std::fs::OpenOptions::new()
        .append(true)
        .open(output_path)?;

    writeln!(f, "{ASM_COUNTER_FILE}")?;

    for i in 0..ASM_COUNTER_SIZE {
        writeln!(f, "DA:{},{}", i + 1, 1)?;
    }

    writeln!(f, "end_of_record")?;

    Ok(())
}
