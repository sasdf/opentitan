use regex::Regex;
use std::error::Error;

use ot_sw_bindgen_rust_bindgen_util as util;

fn get_flags() -> clap::ArgMatches {
    clap::command!()
        .arg(clap::arg!(<input_rs> "Path to the rust source code to be filtered"))
        .arg(clap::arg!(<regex_json> "Path to the regex.json file"))
        .get_matches()
}

#[derive(serde::Deserialize)]
struct Filters {
    regex: Vec<String>,
    literal: Vec<String>,
}

fn read_to_regex(path: &str) -> Result<Option<Regex>, Box<dyn Error>> {
    let content = std::fs::read_to_string(path)?;
    let filters: Filters = serde_json::from_str(&content)?;

    let regexs = filters.regex.into_iter();
    let literals = filters.literal.iter().map(|lit| regex::escape(lit));

    let all_filters: Vec<_> = regexs.chain(literals).collect();

    if all_filters.is_empty() {
        return Ok(None);
    }

    let joined = all_filters.join("|");
    let re = Regex::new(&format!("({joined})"))?;
    Ok(Some(re))
}

fn main() -> Result<(), Box<dyn Error>> {
    let m = get_flags();
    let ast = util::read_to_ast(m.get_one::<String>("input_rs").unwrap())?;
    let re = read_to_regex(m.get_one::<String>("regex_json").unwrap())?;

    util::emit_prefix(&ast);

    for item in ast.items.iter() {
        let item_str = quote::quote!(#item).to_string();
        if re.is_none() || !re.as_ref().unwrap().is_match(&item_str) {
            println!("{}", item_str);
        }
    }

    Ok(())
}
