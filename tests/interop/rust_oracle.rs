//! Run against the immutable xuanrikka branch, without editing its library.
//! Copy to <archived-rust-tree>/examples/interop.rs; build --no-default-features.
use std::{collections::HashMap, env, fs, io::Cursor, path::Path, time::Instant};
use tlg::{PixelLayout, TlgDecoderTrait, TlgEncoderTrait, TlgType, TlgWriter};
use tlg::tlg5::{Tlg5Decoder, Tlg5Encoder};
use tlg::tlg6::{Tlg6Decoder, Tlg6Encoder};

fn layout(c: usize) -> PixelLayout {
    match c { 1 => PixelLayout::Gray, 3 => PixelLayout::Rgb, 4 => PixelLayout::Rgba, _ => panic!("invalid channels") }
}
fn pixels(w: u32, h: u32, c: usize, pattern: &str) -> Vec<u8> {
    let mut out = Vec::with_capacity(w as usize * h as usize * c);
    let mut state = 0xc0ffee12u32;
    for y in 0..h { for x in 0..w { for k in 0..c {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        out.push(match pattern {
            "flat" => if k == 3 { 255 } else { 127 },
            "gradient" => match k { 0 => x as u8, 1 => y as u8, 2 => (x.wrapping_add(y)) as u8, _ => 255 },
            "zero" => 0,
            "random" => (state >> 24) as u8,
            _ => panic!("unknown pattern"),
        });
    }}}
    out
}
fn encode(data: &[u8], w: u32, h: u32, c: usize, format: u8) -> Vec<u8> {
    match format {
        5 => Tlg5Encoder::from_raw(data.to_vec(), layout(c), w, h).encode().unwrap(),
        6 => Tlg6Encoder::from_raw(data.to_vec(), layout(c), w, h).encode().unwrap(),
        _ => panic!("invalid format"),
    }
}
fn decode(data: &[u8], format: u8) -> (Vec<u8>, tlg::ImageInfo) {
    match format {
        5 => Tlg5Decoder::from_reader(Cursor::new(data)).unwrap().decode().unwrap(),
        6 => Tlg6Decoder::from_reader(Cursor::new(data)).unwrap().decode().unwrap(),
        _ => panic!("invalid format"),
    }
}
fn main() {
    let args: Vec<String> = env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("generate") => {
            let dir = Path::new(&args[2]); fs::create_dir_all(dir).unwrap();
            let cases = [("gray_1x1_zero", 1, 1, 1, "zero"),
                ("gray_19x13_random", 19, 13, 1, "random"),
                ("rgb_9x11_gradient", 9, 11, 3, "gradient"),
                ("rgb_32x24_flat", 32, 24, 3, "flat"),
                ("rgba_17x9_random", 17, 9, 4, "random"),
                ("rgba_32x24_gradient", 32, 24, 4, "gradient")];
            for (name, w, h, c, pattern) in cases {
                let raw = pixels(w, h, c, pattern);
                fs::write(dir.join(format!("{name}.raw")), &raw).unwrap();
                for format in [5, 6] {
                    let encoded = encode(&raw, w, h, c, format);
                    let (actual, info) = decode(&encoded, format);
                    assert_eq!(actual, raw, "Rust fixture {name} TLG{format}");
                    assert_eq!((info.width, info.height, info.pixel_layout), (w, h, layout(c)));
                    fs::write(dir.join(format!("{name}.tlg{format}")), &encoded).unwrap();
                }
            }
            // Rust can write SDS, but its TlgReader cannot currently read it.
            let raw = pixels(9, 11, 3, "gradient");
            let tags = HashMap::from([
                ("author".to_owned(), "轩酱".to_owned()),
                ("a:=,文".to_owned(), "value,:=\n中文🙂".to_owned()),
                (String::new(), String::new()),
            ]);
            for (suffix, ty) in [(5, TlgType::Tlg5), (6, TlgType::Tlg6)] {
                let encoded = TlgWriter::from_rgb(raw.clone(), tags.clone(), 9, 11, ty).write().unwrap();
                fs::write(dir.join(format!("metadata.sds{suffix}")), encoded).unwrap();
            }
            println!("generated 6 raw + 12 TLG + 2 SDS fixtures");
        }
        Some("generate_filters") => {
            // One 3x3 block. Preserve the original Rust Golomb streams while
            // replacing only its filter table with each legal literal code.
            // The direct old Rust decoder supplies independent expected pixels.
            let raw = pixels(3, 3, 4, "random");
            let original = encode(&raw, 3, 3, 4, 6);
            let filter_size = u32::from_le_bytes(original[27..31].try_into().unwrap()) as usize;
            let mut bundle = b"TLG6ORCL".to_vec();
            bundle.extend_from_slice(&3u32.to_le_bytes());
            bundle.extend_from_slice(&3u32.to_le_bytes());
            bundle.extend_from_slice(&[4, 32]);
            for code in 0u8..32 {
                let mut variant = original[..27].to_vec();
                variant.extend_from_slice(&2u32.to_le_bytes());
                variant.extend_from_slice(&[0, code]);
                variant.extend_from_slice(&original[31 + filter_size..]);
                let (expected, info) = decode(&variant, 6);
                assert_eq!((info.width, info.height, info.pixel_layout), (3, 3, PixelLayout::Rgba));
                bundle.push(code);
                bundle.extend_from_slice(&(variant.len() as u32).to_le_bytes());
                bundle.extend_from_slice(&(expected.len() as u32).to_le_bytes());
                bundle.extend_from_slice(&variant);
                bundle.extend_from_slice(&expected);
            }
            fs::write(&args[2], &bundle).unwrap();
            println!("generated all 32 legal filter/predictor codes: {} bytes", bundle.len());
        }
        Some("verify") => {
            let format: u8 = args[2].parse().unwrap();
            let w: u32 = args[3].parse().unwrap(); let h: u32 = args[4].parse().unwrap();
            let c: usize = args[5].parse().unwrap();
            let encoded = fs::read(&args[6]).unwrap(); let expected = fs::read(&args[7]).unwrap();
            let (actual, info) = decode(&encoded, format);
            assert_eq!(actual, expected);
            assert_eq!((info.width, info.height, info.pixel_layout), (w, h, layout(c)));
            println!("Rust decoded C++ TLG{format}: {w}x{h}x{c} exact");
        }
        Some("benchmark") => {
            let w: u32 = args.get(2).map(|s| s.parse().unwrap()).unwrap_or(512);
            let h: u32 = args.get(3).map(|s| s.parse().unwrap()).unwrap_or(512);
            let rounds: usize = args.get(4).map(|s| s.parse().unwrap()).unwrap_or(3);
            assert!(w > 0 && h > 0 && rounds > 0);
            println!("implementation,format,pattern,width,height,channels,rounds,encoded_bytes,encode_ms,decode_ms,encode_mib_s,decode_mib_s");
            for pattern in ["flat", "gradient", "random"] { for format in [5, 6] {
                let raw = pixels(w, h, 4, pattern);
                let encoded = encode(&raw, w, h, 4, format);
                assert_eq!(decode(&encoded, format).0, raw);
                let start = Instant::now();
                for _ in 0..rounds { std::hint::black_box(encode(&raw, w, h, 4, format)); }
                let encode_s = start.elapsed().as_secs_f64() / rounds as f64;
                let start = Instant::now();
                for _ in 0..rounds { std::hint::black_box(decode(&encoded, format)); }
                let decode_s = start.elapsed().as_secs_f64() / rounds as f64;
                let mib = raw.len() as f64 / 1048576.0;
                assert!(encode_s > 0.0 && decode_s > 0.0, "timer resolution exceeded; increase rounds");
                println!("rust,{format},{pattern},{w},{h},4,{rounds},{},{},{},{},{}",
                    encoded.len(), encode_s * 1000.0, decode_s * 1000.0, mib / encode_s, mib / decode_s);
            }}
        }
        _ => panic!("usage: interop generate DIR | generate_filters FILE | verify FORMAT W H CHANNELS TLG RAW | benchmark [W H ROUNDS]"),
    }
}
