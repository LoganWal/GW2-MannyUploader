#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RandomError {
    Unavailable,
    TooLarge,
}

/// Platform randomness for generated protocol values. Implementations must not substitute constants.
pub trait RandomSource: Send + Sync {
    fn fill(&self, destination: &mut [u8]) -> Result<(), RandomError>;
}
