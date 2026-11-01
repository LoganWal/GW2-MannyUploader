use serde::de::{
    DeserializeOwned, IgnoredAny, MapAccess, SeqAccess, Visitor, value::MapAccessDeserializer,
};
use serde::{Deserialize, Deserializer};
use std::fmt;
use std::marker::PhantomData;

pub(super) fn decode<T: DeserializeOwned>(bytes: &[u8], maximum: usize) -> Result<T, ()> {
    if bytes.len() > maximum {
        return Err(());
    }
    let text = std::str::from_utf8(bytes).map_err(|_| ())?;
    // IgnoredAny skips Unicode validation in unknown strings. Retain forward-compatible values.
    if !valid_unicode_escapes(bytes) {
        return Err(());
    }
    serde_json::from_str::<Object<T>>(text)
        .map(|Object(value)| value)
        .map_err(|_| ())
}
pub(super) struct Object<T>(pub T);
impl<'de, T: Deserialize<'de>> Deserialize<'de> for Object<T> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct ObjectVisitor<T>(PhantomData<T>);
        impl<'de, T: Deserialize<'de>> Visitor<'de> for ObjectVisitor<T> {
            type Value = Object<T>;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("an object")
            }
            fn visit_map<A: MapAccess<'de>>(self, map: A) -> Result<Self::Value, A::Error> {
                T::deserialize(MapAccessDeserializer::new(map)).map(Object)
            }
        }
        deserializer.deserialize_map(ObjectVisitor(PhantomData))
    }
}
pub(super) struct Text<const LIMIT: usize>(pub String);
pub(super) struct List<T, const LIMIT: usize>(pub Vec<T>);
impl<'de, T: Deserialize<'de>, const LIMIT: usize> Deserialize<'de> for List<T, LIMIT> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct ListVisitor<T, const N: usize>(PhantomData<T>);
        impl<'de, T: Deserialize<'de>, const N: usize> Visitor<'de> for ListVisitor<T, N> {
            type Value = List<T, N>;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("a bounded array")
            }
            fn visit_seq<A: SeqAccess<'de>>(self, mut seq: A) -> Result<Self::Value, A::Error> {
                let mut values = Vec::new();
                while values.len() < N {
                    match seq.next_element()? {
                        Some(value) => values.push(value),
                        None => return Ok(List(values)),
                    }
                }
                if seq.next_element::<IgnoredAny>()?.is_some() {
                    return Err(serde::de::Error::custom("array limit"));
                }
                Ok(List(values))
            }
        }
        deserializer.deserialize_seq(ListVisitor::<T, LIMIT>(PhantomData))
    }
}
impl<'de, const LIMIT: usize> Deserialize<'de> for Text<LIMIT> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct TextVisitor<const N: usize>;
        impl<const N: usize> Visitor<'_> for TextVisitor<N> {
            type Value = Text<N>;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("bounded text")
            }
            fn visit_str<E: serde::de::Error>(self, value: &str) -> Result<Self::Value, E> {
                if value.len() > N {
                    return Err(E::custom("text limit"));
                }
                Ok(Text(value.into()))
            }
        }
        deserializer.deserialize_str(TextVisitor::<LIMIT>)
    }
}
fn valid_unicode_escapes(bytes: &[u8]) -> bool {
    let mut quoted = false;
    let mut index = 0;
    while index < bytes.len() {
        match bytes[index] {
            b'"' => {
                quoted = !quoted;
                index += 1;
            }
            b'\\' if quoted => {
                index += 1;
                if bytes.get(index) == Some(&b'u') {
                    let Some(unit) = unicode_unit(bytes, index + 1) else {
                        return false;
                    };
                    index += 5;
                    if (0xd800..=0xdbff).contains(&unit) {
                        if bytes.get(index..index + 2) != Some(b"\\u") {
                            return false;
                        }
                        let Some(low) = unicode_unit(bytes, index + 2) else {
                            return false;
                        };
                        if !(0xdc00..=0xdfff).contains(&low) {
                            return false;
                        }
                        index += 6;
                    } else if (0xdc00..=0xdfff).contains(&unit) {
                        return false;
                    }
                } else {
                    index += 1;
                }
            }
            _ => index += 1,
        }
    }
    true
}
fn unicode_unit(bytes: &[u8], index: usize) -> Option<u16> {
    bytes
        .get(index..index + 4)?
        .iter()
        .try_fold(0u16, |value, byte| {
            let digit = match byte {
                b'0'..=b'9' => byte - b'0',
                b'a'..=b'f' => byte - b'a' + 10,
                b'A'..=b'F' => byte - b'A' + 10,
                _ => return None,
            };
            Some(value * 16 + u16::from(digit))
        })
}
