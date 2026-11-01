//! Portable lifecycle primitives. Native ABI types stay in the addon crate.

mod callback_gate;

pub use callback_gate::CallbackGate;
