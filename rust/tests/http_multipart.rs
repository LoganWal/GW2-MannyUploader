use manny_uploader::application::*;
use manny_uploader::http::*;

fn part(name: &str, bytes: &[u8]) -> MultipartPart {
    MultipartPart {
        name: name.into(),
        filename: None,
        content_type: None,
        body: Box::new(MemoryBodySource::new(bytes.to_vec()).unwrap()),
    }
}
fn drain(body: &mut dyn HttpBodySource, capacity: usize) -> Result<Vec<u8>, HttpBodyError> {
    let cancel = CancellationSource::default();
    let mut bytes = Vec::new();
    let mut buffer = vec![0; capacity];
    loop {
        let count = body.read(&mut buffer, &cancel.token())?;
        if count == 0 {
            return Ok(bytes);
        }
        bytes.extend_from_slice(&buffer[..count]);
    }
}

#[test]
fn framing_is_exact_across_arbitrary_pull_sizes_and_empty_fields() {
    let expected = b"--test\r\nContent-Disposition: form-data; name=\"token\"\r\n\r\n\r\n--test\r\nContent-Disposition: form-data; name=\"file\"; filename=\"upload.zevtc\"\r\nContent-Type: application/octet-stream\r\n\r\n\0\xffdata\r\n--test--\r\n";
    for capacity in [1, 2, 7, 64, 1024] {
        let mut file = part("file", b"\0\xffdata");
        file.filename = Some("upload.zevtc".into());
        file.content_type = Some("application/octet-stream".into());
        let mut form =
            MultipartFormData::with_boundary(vec![part("token", b""), file], "test").unwrap();
        assert_eq!(form.content_type, "multipart/form-data; boundary=test");
        assert_eq!(form.body.content_length(), expected.len() as u64);
        assert_eq!(
            form.body
                .read(&mut [], &CancellationSource::default().token()),
            Ok(0)
        );
        assert_eq!(drain(&mut *form.body, capacity).unwrap(), expected);
        assert!(drain(&mut *form.body, capacity).unwrap().is_empty());
    }
}

struct Random(u8);
impl RandomSource for Random {
    fn fill(&self, destination: &mut [u8]) -> Result<(), RandomError> {
        match self.0 {
            0 => {
                destination.fill(0xab);
                Ok(())
            }
            1 => Err(RandomError::Unavailable),
            _ => panic!("synthetic RNG failure"),
        }
    }
}
#[test]
fn boundaries_use_random_bytes_and_fail_closed_without_randomness() {
    let form = MultipartFormData::new(vec![part("file", b"data")], &Random(0)).unwrap();
    assert_eq!(
        form.content_type,
        format!(
            "multipart/form-data; boundary=------------------------manny-{}",
            "ab".repeat(16)
        )
    );
    for mode in [1, 2] {
        assert!(matches!(
            MultipartFormData::new(vec![part("file", b"data")], &Random(mode)),
            Err(MultipartError::BoundaryGenerationFailed)
        ));
    }
}

#[test]
fn metadata_rejects_injection_and_enforces_all_bounds() {
    assert!(matches!(
        MultipartFormData::with_boundary(vec![], "b"),
        Err(MultipartError::EmptyParts)
    ));
    assert!(matches!(
        MultipartFormData::with_boundary((0..17).map(|_| part("x", b"")).collect(), "b"),
        Err(MultipartError::TooManyParts)
    ));
    assert!(
        MultipartFormData::with_boundary((0..16).map(|_| part("x", b"")).collect(), "b").is_ok()
    );
    for boundary in [
        "".into(),
        "a".repeat(71),
        "bad boundary".into(),
        "x\r\nInjected".into(),
        "\"quote".into(),
    ] {
        assert!(matches!(
            MultipartFormData::with_boundary(vec![part("x", b"")], &boundary),
            Err(MultipartError::InvalidBoundary)
        ));
    }
    assert!(
        MultipartFormData::with_boundary(vec![part(&"a".repeat(64), b"")], &"b".repeat(70)).is_ok()
    );
    for name in [
        "".into(),
        "a".repeat(65),
        "a\"b".into(),
        "a\r\nb".into(),
        "snow\u{96ea}".into(),
    ] {
        assert!(matches!(
            MultipartFormData::with_boundary(vec![part(&name, b"")], "b"),
            Err(MultipartError::InvalidPartName)
        ));
    }
    for filename in [
        "".into(),
        "a".repeat(256),
        "../a".into(),
        "a\"b".into(),
        "a\r\nb".into(),
    ] {
        let mut p = part("x", b"");
        p.filename = Some(filename);
        assert!(matches!(
            MultipartFormData::with_boundary(vec![p], "b"),
            Err(MultipartError::InvalidFilename)
        ));
    }
    for content_type in [
        "".into(),
        "/x".into(),
        "x/".into(),
        "x/y/z".into(),
        "x/y\r\nz".into(),
        format!("x/{}", "a".repeat(254)),
    ] {
        let mut p = part("x", b"");
        p.content_type = Some(content_type);
        assert!(matches!(
            MultipartFormData::with_boundary(vec![p], "b"),
            Err(MultipartError::InvalidContentType)
        ));
    }
}

struct Source {
    length: u64,
    mode: u8,
}
impl HttpBodySource for Source {
    fn content_length(&self) -> u64 {
        if self.mode == 3 {
            panic!("synthetic length failure")
        }
        self.length
    }
    fn read(&mut self, destination: &mut [u8], _: &StopToken) -> Result<usize, HttpBodyError> {
        match self.mode {
            0 => Ok(0),
            1 => {
                destination.fill(1);
                Ok(destination.len())
            }
            2 => panic!("synthetic read failure"),
            _ => Err(HttpBodyError::new(HttpBodyErrorCode::ReadFailed)),
        }
    }
}
fn source_part(length: u64, mode: u8) -> MultipartPart {
    let mut p = part("x", b"");
    p.body = Box::new(Source { length, mode });
    p
}
#[test]
fn framing_counts_toward_body_limit_without_buffering_source() {
    let empty = MultipartFormData::with_boundary(vec![part("x", b"")], "b")
        .unwrap()
        .body
        .content_length();
    let form = MultipartFormData::with_boundary(
        vec![source_part(MAX_HTTP_REQUEST_BODY_BYTES - empty, 0)],
        "b",
    )
    .unwrap();
    assert_eq!(form.body.content_length(), MAX_HTTP_REQUEST_BODY_BYTES);
    for length in [
        MAX_HTTP_REQUEST_BODY_BYTES - empty + 1,
        MAX_HTTP_REQUEST_BODY_BYTES,
        u64::MAX,
    ] {
        assert!(matches!(
            MultipartFormData::with_boundary(vec![source_part(length, 0)], "b"),
            Err(MultipartError::BodyTooLarge)
        ));
    }
    assert!(matches!(
        MultipartFormData::with_boundary(vec![source_part(1, 3)], "b"),
        Err(MultipartError::InvalidBody)
    ));
}
#[test]
fn child_failures_are_contained_and_permanently_fail_the_form() {
    for (mode, expected) in [
        (0, HttpBodyErrorCode::ReadFailed),
        (1, HttpBodyErrorCode::ReadFailed),
        (2, HttpBodyErrorCode::Panicked),
        (4, HttpBodyErrorCode::ReadFailed),
    ] {
        let mut form = MultipartFormData::with_boundary(vec![source_part(1, mode)], "b").unwrap();
        let error = drain(&mut *form.body, 64).unwrap_err();
        assert_eq!(error.code, expected);
        assert_eq!(
            form.body
                .read(&mut [0; 1], &CancellationSource::default().token()),
            Err(error)
        );
    }
}
#[test]
fn cancellation_remains_terminal_with_a_fresh_token() {
    let mut form = MultipartFormData::with_boundary(vec![part("x", b"data")], "b").unwrap();
    let cancel = CancellationSource::default();
    cancel.request_stop();
    let error = form.body.read(&mut [0; 1], &cancel.token()).unwrap_err();
    assert_eq!(error.code, HttpBodyErrorCode::Cancelled);
    assert_eq!(
        form.body
            .read(&mut [0; 1], &CancellationSource::default().token()),
        Err(error)
    );
}
