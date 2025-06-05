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

const BUILD_ID_SIZE: usize = 20;
const PRF_MAGIC: u64 = 0xff6c70726f665281;
const OTC_MAGIC: u64 = 0x7265766f43544f81; // File magic: \x81OTCover
const PRF_VERSION: u64 = 8;
const PRF_DATA_ENTRY_SIZE: u64 = 40;
const VARIANT_MASK_BYTE_COVERAGE: u64 = (0x1 << 60);

macro_rules! debug_log {
    ($($arg:tt)*) => {
        if env::var("VERBOSE_COVERAGE").is_ok() {
            eprintln!($($arg)*);
        }
    };
}

fn search_by_extension(dir: &PathBuf, extension: &str) -> Vec<PathBuf> {
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

#[derive(AsBytes, Debug, Default)]
#[repr(C)]
struct ProfileHeader {
    Magic: u64,
    Version: u64,
    BinaryIdsSize: u64,
    NumData: u64,
    PaddingBytesBeforeCounters: u64,
    NumCounters: u64,
    PaddingBytesAfterCounters: u64,
    NamesSize: u64,
    CountersDelta: u64,
    NamesDelta: u64,
    ValueKindLast: u64,
}

struct ProfileCounter {
    build_id: String,
    cnts: Vec<u8>,
}

struct ProfileData {
    build_id: String,
    elf: PathBuf,
    file_name: String,
    header: ProfileHeader,
    cnts_size: u64,
    data: Vec<u8>,
    names: Vec<u8>,
}

fn process_elf(path: &PathBuf) -> Result<ProfileData> {
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

fn decompress(path: &PathBuf) -> Result<ProfileCounter> {
    let mut f = std::fs::File::open(path)?;

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
                    f.read_exact(&mut pad)?;
                    u32::from_le_bytes(pad) as usize
                }
                // Any other value is the padding length itself.
                _ => byte[0] as usize,
            };
            cnts.resize(cnts.len() + pad, tag);
        } else {
            // Regular data byte.
            cnts.push(byte[0]);
        }
    }

    if cnts.len() < BUILD_ID_SIZE {
        bail!("Missing build id in the decompressed data");
    }

    let (build_id, cnts) = cnts.split_at(BUILD_ID_SIZE);

    Ok(ProfileCounter {
        build_id: hex::encode(build_id),
        cnts: cnts.to_vec(),
    })
}

fn process_profraw(path: &PathBuf, profile_map: &HashMap<String, ProfileData>) -> Result<()> {
    let ProfileCounter { build_id, cnts } = decompress(path)?;

    if cnts.len() < 8 {
        bail!("Input profraw file is too short.");
    }

    let (magic, cnts) = cnts.split_at(8);
    let magic = u64::from_le_bytes(magic.try_into()?);
    if magic == PRF_MAGIC {
        // Full profraw, save it directly.
        std::fs::write(path, cnts)?;
        return Ok(());
    }

    if magic != OTC_MAGIC {
        bail!("Unknown profraw file magic bytes.");
    }

    let (version, cnts) = cnts.split_at(8);
    let version = u64::from_le_bytes(version.try_into()?);
    let cnts_width = if (version & VARIANT_MASK_BYTE_COVERAGE != 0) {1} else {8};

    // Counters only, try to correlate with elf data.
    let profile = match profile_map.get(&build_id) {
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

    if cnts.len() % cnts_width != 0 {
        bail!("Invalid __llvm_prf_cnts section size");
    }

    let header = ProfileHeader{
        Version: version,
        NumCounters: (cnts.len() / cnts_width) as u64,
        ..profile.header
    };
    debug_log!("{:#?}", header);
    assert_eq!(profile.data.len() as u64, header.NumData * PRF_DATA_ENTRY_SIZE);
    assert_eq!(profile.names.len() as u64, header.NamesSize);

    let mut f = std::fs::File::create(path)?;
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

fn merge_profraw(profraw_files: &Vec<PathBuf>, profdata_file: &PathBuf) {
    let llvm_profdata = &env::var("LLVM_PROFDATA").unwrap();
    debug_log!("llvm_profdata: {llvm_profdata}");

    // "${LLVM_PROFDATA}" merge -output "${profdata_file}" "${COVERAGE_DIR}"/*.profraw
    let mut llvm_profdata_cmd = process::Command::new(llvm_profdata);
    llvm_profdata_cmd
        .arg("merge")
        .arg("--sparse")
        .args(profraw_files)
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

fn llvm_cov_export(format: &str, profdata_file: &PathBuf, objects: &Vec<String>, output_file: &PathBuf) {
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

fn collect_objects() -> Vec<String> {
    let coverage_manifest = PathBuf::from(env::var("COVERAGE_MANIFEST").unwrap());

    // Collect all objects from the coverage_manifest.
    fs::read_to_string(coverage_manifest)
        .unwrap()
        .lines()
        .filter(|path| path.ends_with(".gcno"))
        .map(|path| PathBuf::from(path))
        .filter_map(|path| std::fs::canonicalize(path).ok())
        .flat_map(|mut path| {
            path.set_extension("o");
            let path = path.into_os_string().into_string().unwrap();
            ["-object".to_string(), path]
        })
        .collect()
}

fn main() -> Result<()> {
    let coverage_dir = PathBuf::from(env::var("COVERAGE_DIR").unwrap());
    let execroot = PathBuf::from(env::var("ROOT").unwrap());
    let mut runfiles_dir = PathBuf::from(env::var("RUNFILES_DIR").unwrap());

    if !runfiles_dir.is_absolute() {
        runfiles_dir = execroot.join(runfiles_dir);
    }

    debug_log!("ROOT: {}", execroot.display());
    debug_log!("RUNFILES_DIR: {}", runfiles_dir.display());

    let lcov_output_file = coverage_dir.join("coverage.dat");
    let json_output_file = coverage_dir.join("coverage.json");
    let profdata_file = coverage_dir.join("coverage.profdata");


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

    let objects = collect_objects();

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
        process_profraw(path, &profile_map).unwrap();
    }

    merge_profraw(&profraw_files, &profdata_file);
    llvm_cov_export("lcov", &profdata_file, &objects, &lcov_output_file);
    llvm_cov_export("text", &profdata_file, &objects, &json_output_file);

    // Destroy the intermediate binary file so lcov_merger doesn't parse it twice.
    debug_log!("Cleaning up {}", profdata_file.display());
    fs::remove_file(profdata_file).unwrap();

    debug_log!("Success!");
    Ok(())
}
