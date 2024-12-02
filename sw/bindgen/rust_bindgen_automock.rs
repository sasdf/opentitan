use quote::ToTokens;
use std::error::Error;

use ot_sw_bindgen_rust_bindgen_util as util;

fn get_flags() -> clap::ArgMatches {
    clap::command!()
        .arg(clap::arg!(<import_rs> "The import.rs headers to be prepended"))
        .arg(clap::arg!(<input_rs> "Path to the rust binding source to be mocked"))
        .get_matches()
}

pub fn gather_foreign_functions(externs: &Vec<syn::ItemForeignMod>) -> Vec<syn::ForeignItemFn> {
    let mut output = Vec::new();
    for foreign_mod in externs {
        for foreign_item in &foreign_mod.items {
            match foreign_item {
                syn::ForeignItem::Fn(item) => output.push(item.clone()),
                syn::ForeignItem::Static(_) => {
                    // automock does not support static var.
                }
                _ => eprintln!("Unexpected foreign item {}", foreign_item.to_token_stream()),
            }
        }
    }
    output
}

fn main() -> Result<(), Box<dyn Error>> {
    let m = get_flags();
    let header = util::read_to_ast(m.get_one::<String>("import_rs").unwrap())?;
    let ast = util::read_to_ast(m.get_one::<String>("input_rs").unwrap())?;

    let (externs, _) = util::group_externs(&ast);
    let functions = gather_foreign_functions(&externs);
    let mock_contexts: Vec<proc_macro2::TokenStream> = functions
        .iter()
        .map(|func| (func.sig.ident.to_string() + "_context").parse().unwrap())
        .collect();

    let output = quote::quote! {
        #[mockall::automock]
        #[allow(unused)]
        pub mod __ot_automock_extern__ {
            #header

            extern "C" {
                #(#functions)*
            }
        }

        // Re-export the mocking context.
        #(
            pub use self::mock___ot_automock_extern__::#mock_contexts;
        )*
    };

    println!("{}", output);

    Ok(())
}
