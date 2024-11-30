use std::error::Error;

use ot_util_design_rust_bindgen_util as util;

fn get_flags() -> clap::ArgMatches {
    clap::command!()
        .arg(clap::arg!(<import_rs> "The import.rs headers to be prepended"))
        .arg(clap::arg!(<base_rs> "The base.rs base bindings to be removed"))
        .arg(clap::arg!(<bind_rs> "The bind.rs new bindings"))
        .get_matches()
}

fn main() -> Result<(), Box<dyn Error>> {
    let m = get_flags();
    let header = std::fs::read_to_string(m.get_one::<String>("import_rs").unwrap())?;
    let base_ast = util::read_to_ast(m.get_one::<String>("base_rs").unwrap())?;
    let bind_ast = util::read_to_ast(m.get_one::<String>("bind_rs").unwrap())?;
    let bind_iter = bind_ast.items.iter();
    let mut base_iter = base_ast.items.iter().peekable();

    util::emit_prefix(&bind_ast);

    println!("{header}");

    for bind_item in bind_iter {
        // Skip if bind item matches base item.
        if let Some(base_item) = base_iter.peek() {
            if base_item == &bind_item {
                base_iter.next();
                continue;
            }
        }

        // Found new items, print it out!
        println!("{}", quote::quote!(#bind_item).to_string());
    }

    assert_eq!(
        base_iter.peek(),
        None,
        "something is missing in the generated binding."
    );

    Ok(())
}
