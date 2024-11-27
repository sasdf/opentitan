use std::error::Error;

pub fn read_to_ast(path: &str) -> Result<syn::File, Box<dyn Error>> {
    let content = std::fs::read_to_string(path)?;
    let ast = syn::parse_file(&content)?;
    Ok(ast)
}

pub fn emit_prefix(ast: &syn::File) {
    if let Some(shebang) = &ast.shebang {
        println!("{}", shebang);
    }

    for item in ast.attrs.iter() {
        let item_str = quote::quote!(#item).to_string();
        println!("{}", item_str);
    }
}
