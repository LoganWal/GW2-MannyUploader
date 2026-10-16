use manny_core::application::SecretValue;
use nexus::imgui::sys as imgui;
use std::{ffi::CString, marker::PhantomData, ptr};
use zeroize::{Zeroize, Zeroizing};

#[cfg(test)]
pub(super) static TRACE: std::sync::Mutex<Vec<(String, [f32; 4])>> =
    std::sync::Mutex::new(Vec::new());

pub(super) struct TextBuffer(Zeroizing<Vec<u8>>);
impl TextBuffer {
    pub fn new(maximum_bytes: usize) -> Self {
        Self(Zeroizing::new(vec![0; maximum_bytes + 1]))
    }
    pub fn set(&mut self, value: &str) {
        self.0.as_mut_slice().zeroize();
        let mut end = value.len().min(self.0.len() - 1);
        while !value.is_char_boundary(end) {
            end -= 1;
        }
        let value = &value.as_bytes()[..end];
        let end = value
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(value.len());
        self.0[..end].copy_from_slice(&value[..end]);
    }
    pub fn text(&self) -> &str {
        let end = self
            .0
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(self.0.len());
        std::str::from_utf8(&self.0[..end]).unwrap_or("")
    }
    pub fn take_secret(&mut self) -> SecretValue {
        let length = self.text().len();
        let capacity = self.0.len();
        let mut old = std::mem::replace(&mut self.0, Zeroizing::new(vec![0; capacity]));
        old.truncate(length);
        SecretValue::from_bytes(std::mem::take(&mut *old))
    }
    pub fn clear(&mut self) {
        self.0.as_mut_slice().zeroize();
    }
}

fn label(text: &str) -> CString {
    CString::new(text.replace('\0', " ")).expect("NUL bytes removed")
}

/// This token cannot leave the render thread or outlive its admitted callback.
pub(super) struct Frame(PhantomData<*mut ()>);
impl Frame {
    /// # Safety
    /// The caller must be inside a Nexus ImGui 1.80 frame, on its render thread, with callback
    /// admission held. Nexus owns the current context, which must outlive this token and its scopes.
    pub unsafe fn new() -> Self {
        Self(PhantomData)
    }

    pub fn text(&self, value: &str) {
        // SAFETY: Wrapping is scoped to this item in the active window or table column.
        unsafe {
            imgui::igPushTextWrapPos(0.0);
        }
        let _scope = Scope(End::Wrap);
        // SAFETY: The frame contract provides a live context. Both pointers bound this UTF-8 slice.
        unsafe {
            imgui::igTextUnformatted(
                value.as_ptr().cast(),
                value.as_ptr().add(value.len()).cast(),
            );
        }
        #[cfg(test)]
        self.trace(value);
    }
    pub fn colored(&self, value: &str, color: [f32; 4]) {
        // SAFETY: The frame is active and the style entry is balanced even if rendering unwinds.
        unsafe {
            imgui::igPushStyleColor_Vec4(
                imgui::ImGuiCol_Text as i32,
                imgui::ImVec4 {
                    x: color[0],
                    y: color[1],
                    z: color[2],
                    w: color[3],
                },
            );
        }
        let _scope = Scope(End::Color);
        self.text(value);
    }
    pub fn warning(&self, value: &str) {
        self.colored(value, [1.0, 0.35, 0.3, 1.0]);
    }
    pub fn same_line(&self) {
        // SAFETY: Called only within the active frame.
        unsafe {
            imgui::igSameLine(0.0, -1.0);
        }
    }
    pub fn separator(&self) {
        // SAFETY: Called only within the active frame.
        unsafe {
            imgui::igSeparator();
        }
    }
    pub fn button(&self, title: &str, enabled: bool) -> bool {
        if !enabled {
            self.colored(title, [0.5, 0.5, 0.5, 1.0]);
            return false;
        }
        let title = label(title);
        // SAFETY: The terminated label remains alive and the current frame is valid.
        let clicked = unsafe { imgui::igButton(title.as_ptr(), imgui::ImVec2 { x: 0.0, y: 0.0 }) };
        #[cfg(test)]
        self.trace(title.to_str().unwrap());
        clicked
    }
    pub fn checkbox(&self, title: &str, value: &mut bool, enabled: bool) -> bool {
        if !enabled {
            let title = title.split("##").next().unwrap_or("");
            self.colored(
                &format!("[{}] {title}", if *value { "x" } else { " " }),
                [0.5, 0.5, 0.5, 1.0],
            );
            return false;
        }
        let title = label(title);
        // SAFETY: The live boolean and terminated label are borrowed only for this widget call.
        let changed = unsafe { imgui::igCheckbox(title.as_ptr(), value) };
        #[cfg(test)]
        self.trace(title.to_str().unwrap());
        changed
    }
    pub fn input(
        &self,
        title: &str,
        buffer: &mut TextBuffer,
        password: bool,
        enabled: bool,
    ) -> bool {
        let title = label(title);
        let mut flags = if password {
            imgui::ImGuiInputTextFlags_Password
        } else {
            0
        };
        if !enabled {
            flags |= imgui::ImGuiInputTextFlags_ReadOnly;
        }
        // SAFETY: Fixed, initialized, NUL-terminated storage has one extra byte for termination.
        // No resize callbacks are used and the buffer remains alive throughout the call.
        unsafe {
            imgui::igInputText(
                title.as_ptr(),
                buffer.0.as_mut_ptr().cast(),
                buffer.0.len(),
                flags as i32,
                None,
                ptr::null_mut(),
            ) && enabled
        }
    }
    pub fn integer(
        &self,
        title: &str,
        value: &mut u32,
        range: std::ops::RangeInclusive<u32>,
        enabled: bool,
    ) -> bool {
        if !enabled {
            self.text(&format!("{title}: {value}"));
            return false;
        }
        let title = label(title);
        let mut edited = (*value).min(i32::MAX as u32) as i32;
        // SAFETY: The integer and label are valid for the call within the current frame.
        if unsafe { imgui::igInputInt(title.as_ptr(), &mut edited, 1, 10, 0) } {
            *value = (edited.max(0) as u32).clamp(*range.start(), *range.end());
            true
        } else {
            false
        }
    }
    pub fn combo(&self, title: &str, preview: &str, enabled: bool, body: impl FnOnce()) {
        if !enabled {
            self.text(&format!("{title}: {preview}"));
            return;
        }
        let title = label(title);
        let preview = label(preview);
        // SAFETY: Live labels and active frame. EndCombo is called only when BeginCombo succeeds.
        if unsafe { imgui::igBeginCombo(title.as_ptr(), preview.as_ptr(), 0) } {
            let _scope = Scope(End::Combo);
            body();
        }
    }
    pub fn selectable(&self, title: &str, selected: bool) -> bool {
        let title = label(title);
        // SAFETY: The label remains alive for this widget in the current frame.
        unsafe {
            imgui::igSelectable_Bool(
                title.as_ptr(),
                selected,
                0,
                imgui::ImVec2 { x: 0.0, y: 0.0 },
            )
        }
    }
    pub fn id(&self, id: &str, body: impl FnOnce()) {
        // SAFETY: Explicit slice bounds avoid NUL conversion and the scope balances the ID stack.
        unsafe {
            imgui::igPushID_StrStr(id.as_ptr().cast(), id.as_ptr().add(id.len()).cast());
        }
        let _scope = Scope(End::Id);
        body();
    }
    pub fn window(&self, visible: &mut bool, body: impl FnOnce()) {
        // SAFETY: Set only the initial size. Nexus retains user changes in its existing context.
        unsafe {
            imgui::igSetNextWindowSize(
                imgui::ImVec2 {
                    x: 1100.0,
                    y: 500.0,
                },
                imgui::ImGuiCond_FirstUseEver as i32,
            );
        }
        // SAFETY: Nexus owns the current frame and this boolean is live for Begin. Every Begin,
        // including a collapsed window, is paired with End by the scope.
        let draw = unsafe { imgui::igBegin(c"MannyUploader".as_ptr(), visible, 0) };
        let _scope = Scope(End::Window);
        if draw {
            body();
        }
    }
    pub fn table(&self, columns: &[&str], body: impl FnOnce()) {
        // SAFETY: Column count is bounded by the renderer and the identifier is static.
        if unsafe {
            imgui::igBeginTable(
                c"recent_logs".as_ptr(),
                columns.len() as i32,
                (imgui::ImGuiTableFlags_Borders
                    | imgui::ImGuiTableFlags_RowBg
                    | imgui::ImGuiTableFlags_Resizable) as i32,
                imgui::ImVec2 { x: 0.0, y: 0.0 },
                0.0,
            )
        } {
            let _scope = Scope(End::Table);
            for title in columns {
                let width = match *title {
                    "Select" => 45.0,
                    "Detected" => 195.0,
                    "Actions" => 95.0,
                    _ => 0.0,
                };
                let flags = if width > 0.0 {
                    imgui::ImGuiTableColumnFlags_WidthFixed
                } else {
                    imgui::ImGuiTableColumnFlags_WidthStretch
                };
                let title = label(title);
                // SAFETY: This table is active and setup labels remain alive for each call.
                unsafe {
                    imgui::igTableSetupColumn(title.as_ptr(), flags as i32, width, 0);
                }
            }
            // SAFETY: Setup is complete for the active table.
            unsafe {
                imgui::igTableHeadersRow();
            }
            body();
        }
    }
    pub fn next_row(&self) {
        // SAFETY: Renderer calls this only inside table's body closure.
        unsafe {
            imgui::igTableNextRow(0, 0.0);
        }
    }
    pub fn column(&self) {
        // SAFETY: Renderer calls this only inside table's body closure.
        unsafe {
            imgui::igTableNextColumn();
        }
    }
    pub fn copy(&self, text: &str) {
        let text = label(text);
        // SAFETY: The current host's clipboard callback copies the live terminated text during
        // this explicit button action. No pointer is retained by the addon.
        unsafe {
            imgui::igSetClipboardText(text.as_ptr());
        }
    }
    #[cfg(test)]
    fn trace(&self, label: &str) {
        let mut min = imgui::ImVec2 { x: 0.0, y: 0.0 };
        let mut max = min;
        // SAFETY: Test tracing runs immediately after a widget in the active frame.
        unsafe {
            imgui::igGetItemRectMin(&mut min);
            imgui::igGetItemRectMax(&mut max);
        }
        TRACE
            .lock()
            .unwrap()
            .push((label.into(), [min.x, min.y, max.x, max.y]));
    }
}

enum End {
    Wrap,
    Color,
    Combo,
    Id,
    Window,
    Table,
}
struct Scope(End);
impl Drop for Scope {
    fn drop(&mut self) {
        // SAFETY: Each scope is created only after its matching push/begin in an admitted frame.
        unsafe {
            match self.0 {
                End::Wrap => imgui::igPopTextWrapPos(),
                End::Color => imgui::igPopStyleColor(1),
                End::Combo => imgui::igEndCombo(),
                End::Id => imgui::igPopID(),
                End::Window => imgui::igEnd(),
                End::Table => imgui::igEndTable(),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn text_buffers_preserve_capacity_utf8_and_termination() {
        let mut buffer = TextBuffer::new(4);
        buffer.set("ab\u{96ea}");
        assert_eq!(buffer.text(), "ab");
        assert_eq!(buffer.0.len(), 5);
        buffer.set("\u{96ea}a");
        assert_eq!(buffer.text(), "\u{96ea}a");
        buffer.set("a\0hidden");
        assert_eq!(buffer.text(), "a");
        buffer.clear();
        assert_eq!(&*buffer.0, &[0; 5]);
    }
    #[test]
    fn credential_transfer_clears_reusable_input_storage() {
        let mut buffer = TextBuffer::new(128);
        buffer.set("synthetic-key");
        let secret = buffer.take_secret();
        assert_eq!(secret.expose(), b"synthetic-key");
        assert_eq!(buffer.0.len(), 129);
        assert!(buffer.0.iter().all(|byte| *byte == 0));
        buffer.set("next");
        assert_eq!(buffer.text(), "next");
    }
}
