use proc_macro::TokenStream;
use proc_macro2::Span;
use quote::quote;
use quote::ToTokens;

use ot_sw_bindgen_rust_bindgen_util as util;

/// Load the generated bindgen file.
fn load_bindgen() -> syn::File {
    let path = std::env::var("OT_BINDGEN_RS_PATH").expect("Bindings path is not in the environ");
    util::read_to_ast(&path).expect("Failed to load generated bindings")
}

/// Converts a function signature to its pointer type.
fn sig_to_type(sig: &syn::Signature) -> String {
    const IDENT: &str = "_OT_EMPTY_IDENT_";
    let mut ptr_sig = sig.clone();
    ptr_sig.ident = syn::Ident::new(IDENT, Span::call_site());
    ptr_sig.to_token_stream().to_string().replacen(IDENT, "", 1)
}

/// Codegen type checks for the extern functions / variables.
fn generate_type_assertions(externs: &Vec<syn::ItemForeignMod>) -> proc_macro2::TokenStream {
    let mut checkers = Vec::new();

    for foreign_mod in externs {
        for foreign_item in &foreign_mod.items {
            match foreign_item {
                syn::ForeignItem::Fn(item) => {
                    let abi = foreign_mod.abi.to_token_stream();
                    let name = item.sig.ident.to_string();
                    let ty = sig_to_type(&item.sig);
                    checkers.push(format!(
                        r#"
                        let _: unsafe {abi} {ty} = self::{name};
                        let _: unsafe {abi} {ty} = crate::{name};
                    "#
                    ));
                }
                syn::ForeignItem::Static(item) => {
                    let name = item.ident.to_string();
                    let ty = item.ty.to_token_stream();
                    checkers.push(format!(
                        r#"
                        let _: {ty} = self::{name};
                        let _: {ty} = crate::{name};
                    "#
                    ));
                }
                _ => eprintln!("Unexpected foreign item {}", foreign_item.to_token_stream()),
            }
        }
    }

    checkers
        .join("\n")
        .parse()
        .expect("Failed to parse generated checkers")
}

#[proc_macro]
pub fn include(_: TokenStream) -> TokenStream {
    let ast = load_bindgen();

    let (externs, others) = util::group_externs(&ast);

    let type_assertions = generate_type_assertions(&externs);

    let output = quote! {
        #(#others)*

        // Wrap externs in a mod to prevent implementation redefine.
        #[allow(unused)]
        mod __ot_bindgen_extern__ {
            use super::*;
            #(#externs)*

            unsafe fn __ot_bindgen_assertions__ () {
                #type_assertions
            }
        }

        // Re-export from the wrapped mod.
        pub use self::__ot_bindgen_extern__::*;
    };

    output.into()
}
