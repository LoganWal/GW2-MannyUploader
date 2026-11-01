pub const NORMAL: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/upload.png"));
pub const IDLE: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/idle.png"));
pub const TWITCH: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/twitch.png"));
pub const DETAILED_WVW: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/detailed_wvw.png"));
pub const WINGMAN: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/wingman.png"));
pub const DONBOT: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/donbot.png"));

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_variants_change_only_bottom_line_palette() {
        let icons = [IDLE, NORMAL, DETAILED_WVW, WINGMAN, DONBOT, TWITCH];
        let mut colors = Vec::new();
        for icon in icons {
            assert_eq!(&icon[..8], b"\x89PNG\r\n\x1a\n");
            let mut offset = 8;
            let mut palette = &[][..];
            while offset < icon.len() {
                let length =
                    u32::from_be_bytes(icon[offset..offset + 4].try_into().unwrap()) as usize;
                let kind = &icon[offset + 4..offset + 8];
                let data = &icon[offset + 8..offset + 8 + length];
                assert_eq!(
                    crc32fast::hash(&icon[offset + 4..offset + 8 + length]),
                    u32::from_be_bytes(
                        icon[offset + 8 + length..offset + 12 + length]
                            .try_into()
                            .unwrap()
                    )
                );
                if kind == b"PLTE" {
                    palette = data;
                }
                if kind == b"IDAT" {
                    // Stored DEFLATE payload has a two-byte zlib header and five-byte block header.
                    let pixels = &data[7..data.len() - 4];
                    assert_eq!(pixels.len(), 32 * 33);
                    for y in 0..32 {
                        assert_eq!(pixels[y * 33], 0);
                        for x in 0..32 {
                            let pixel = pixels[y * 33 + 1 + x];
                            if x == 0 || x == 31 || y == 0 || y == 31 {
                                assert_eq!(pixel, 0, "border must be transparent");
                            }
                            assert_eq!(pixel == 2, (26..=28).contains(&y) && (6..=25).contains(&x));
                        }
                    }
                    assert_eq!(data, idat(IDLE));
                }
                offset += length + 12;
            }
            assert_eq!(&palette[3..6], &[244, 246, 254]);
            colors.push(palette[6..9].to_vec());
        }
        colors.sort();
        colors.dedup();
        assert_eq!(colors.len(), 6);
    }

    fn idat(icon: &[u8]) -> &[u8] {
        let mut offset = 8;
        loop {
            let length = u32::from_be_bytes(icon[offset..offset + 4].try_into().unwrap()) as usize;
            if &icon[offset + 4..offset + 8] == b"IDAT" {
                return &icon[offset + 8..offset + 8 + length];
            }
            offset += length + 12;
        }
    }
}
