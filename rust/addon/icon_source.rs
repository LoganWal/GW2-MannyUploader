// Code-owned geometry keeps the small Nexus icon crisp and every status variant identical.
pub const VARIANTS: [(&str, [u8; 3]); 6] = [
    ("idle", [244, 246, 254]),
    ("upload", [65, 155, 235]),
    ("detailed_wvw", [217, 195, 62]),
    ("wingman", [176, 120, 35]),
    ("donbot", [41, 189, 211]),
    ("twitch", [145, 70, 255]),
];

pub fn png(line: [u8; 3]) -> Vec<u8> {
    let mut pixels = Vec::with_capacity(32 * 33);
    for y in 0_i32..32 {
        pixels.push(0); // PNG filter: none.
        for x in 0_i32..32 {
            let head = (4..=13).contains(&y) && (x - 15).abs() <= y - 4;
            let stem = (12..=19).contains(&x) && (12..=23).contains(&y);
            let base = (6..=25).contains(&x) && (26..=28).contains(&y);
            pixels.push(if base { 2 } else { u8::from(head || stem) });
        }
    }
    // One stored DEFLATE block avoids compression dependencies for this fixed 32x32 asset.
    let length = pixels.len() as u16;
    let mut zlib = vec![0x78, 0x01, 0x01];
    zlib.extend(length.to_le_bytes());
    zlib.extend((!length).to_le_bytes());
    zlib.extend(&pixels);
    let (mut a, mut b) = (1_u32, 0_u32);
    for byte in pixels {
        a = (a + u32::from(byte)) % 65521;
        b = (b + a) % 65521;
    }
    zlib.extend(((b << 16) | a).to_be_bytes());
    let mut png = b"\x89PNG\r\n\x1a\n".to_vec();
    chunk(
        &mut png,
        b"IHDR",
        &[0, 0, 0, 32, 0, 0, 0, 32, 8, 3, 0, 0, 0],
    );
    chunk(
        &mut png,
        b"PLTE",
        &[0, 0, 0, 244, 246, 254, line[0], line[1], line[2]],
    );
    chunk(&mut png, b"tRNS", &[0]);
    chunk(&mut png, b"IDAT", &zlib);
    chunk(&mut png, b"IEND", &[]);
    png
}

fn chunk(png: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
    png.extend((data.len() as u32).to_be_bytes());
    png.extend(kind);
    png.extend(data);
    let mut crc = !0_u32;
    for byte in kind.iter().chain(data) {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb88320 & 0_u32.wrapping_sub(crc & 1));
        }
    }
    png.extend((!crc).to_be_bytes());
}
