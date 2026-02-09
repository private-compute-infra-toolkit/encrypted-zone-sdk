// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use proc_macro::TokenStream;
use quote::quote;
use syn::{parse_macro_input, Fields, ItemStruct};

/// Augments a struct with an SDK client field and accessors.
///
/// This attribute macro injects a `client` field of type `tokio::sync::OnceCell<std::sync::Arc<IsolateEzBridgeSdkClient>>`
/// into the decorated struct. Since `OnceCell` implements `Default`, the `client` field can be
/// simply initialized using `..Default::default()`.
///
/// It also generates an `impl` block with the following methods:
///
/// * `set_client(client: std::sync::Arc<IsolateEzBridgeSdkClient>)`: Sets the client. Panics if already set.
/// * `get_client() -> std::sync::Arc<IsolateEzBridgeSdkClient>`: Gets the client. Panics if not set.
///
/// Use this macro on your service logic struct to easily manage the SDK client needed for
/// interacting with the Encrypted Zone.
///
/// This macro is designed to work seamlessly with the `create_isolate_server_with_client!` macro,
/// which automatically calls `set_client` on the service logic instance during server initialization.
///
/// # Note
///
/// This macro only supports structs with named fields. Tuple structs and unit structs
/// (e.g., `struct MyStruct;`) are not supported.
/// You need to manually import the `IsolateEzBridgeSdkClient` to use this macro.
///
/// # Arguments
///
/// * `_attr` - Unused.
/// * `item` - The struct definition to augment.
///
/// # Returns
///
/// The modified struct definition and the generated `impl` block.
///
/// # Example
///
/// ```rust,ignore
///
/// use rust_core::with_sdk_client;
/// use rust_core::create_isolate_server_with_client;
/// use rust_core::IsolateEzBridgeSdkClient;
/// use std::sync::Arc;
///
/// #[with_sdk_client]
/// #[derive(Default)]
/// struct MyServiceLogic {
///     // other fields
/// }
///
/// impl MyServiceLogic {
///     fn new() -> Self {
///         Self {
///             // initialize other fields
///             ..Default::default()
///         }
///     }
///
///     fn use_client(&self) {
///         let client = self.get_client();
///         // use the client
///     }
/// }
///
/// let logic = std::sync::Arc::new(MyServiceLogic::new());
///
/// let server = create_isolate_server_with_client! {
///     MyServiceIsolateRpcService => logic
/// };
/// ```
#[proc_macro_attribute]
pub fn with_sdk_client(_attr: TokenStream, item: TokenStream) -> TokenStream {
    // 1. Parse the input struct
    let mut input = parse_macro_input!(item as ItemStruct);
    let struct_name = &input.ident; // Capture the struct name for the impl block

    // 2. Add the 'client' field
    if let Fields::Named(ref mut fields) = input.fields {
        let new_field: syn::Field = syn::parse_quote! {
            // We use fully qualified paths to ensure safety regardless of imports
            client: tokio::sync::OnceCell<std::sync::Arc<IsolateEzBridgeSdkClient>>
        };
        fields.named.push(new_field);
    } else {
        return syn::Error::new_spanned(
            input,
            "The `with_client` macro only supports structs with named fields.",
        )
        .to_compile_error()
        .into();
    }

    // 3. Generate the implementation block
    // We expect() inside the methods because the user signatures do not return Results/Options.
    let expanded = quote! {
        #input

        impl #struct_name {
            pub fn set_client(&self, client: std::sync::Arc<IsolateEzBridgeSdkClient>) {
                self.client.set(client)
                    .expect("Failed to set client: It was already initialized.");
            }

            pub fn get_client(&self) -> std::sync::Arc<IsolateEzBridgeSdkClient> {
                self.client.get()
                    .expect("Failed to get client: It has not been initialized yet.")
                    .clone()
            }
        }
    };

    TokenStream::from(expanded)
}
