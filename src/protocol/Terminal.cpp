#include "protocol/Terminal.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace btpdev {

std::string render_command_output(const char* text, std::uint8_t status) {
    const std::string input = text != nullptr ? text : "";
    std::string out;
    out.reserve(input.size() + 16U);
    std::size_t start = 0U;
    while (start <= input.size()) {
        const std::size_t nl = input.find('\n', start);
        const bool last = nl == std::string::npos;
        std::size_t end = last ? input.size() : nl;
        while (end > start && (input[end - 1U] == '\r' || input[end - 1U] == ' ')) --end;
        if (end > start) {
            out += "! ";
            out.append(input, start, end - start);
            out += "\r\n";
        }
        if (last) break;
        start = nl + 1U;
    }
    if (out.size() > Terminal::kMaxCommandOut) {
        out.resize(Terminal::kMaxCommandOut);
        out += "\r\n! [output truncated]\r\n";
    }
    if (status != 0U) {
        char note[32];
        std::snprintf(note, sizeof(note), "! [status=%u]\r\n", static_cast<unsigned>(status));
        out += note;
    }
    return out;
}

Terminal::Terminal() noexcept = default;

void Terminal::configure(ShellLineEditor::CompletionProvider completion, SubmitFn submit,
                         EmitFn emit, void* context, const char* prompt, std::size_t max_chunk) {
    completion_ = std::move(completion);
    submit_ = submit;
    emit_ = emit;
    context_ = context;
    prompt_ = prompt != nullptr ? prompt : "";
    max_chunk_ = max_chunk > 0U ? max_chunk : 200U;
    reset();
}

void Terminal::reset() {
    for (Slot& slot : slots_) slot.used = false;
}

Terminal::Slot* Terminal::find(std::uint32_t source_id, std::uint32_t boot_id) {
    for (Slot& slot : slots_) {
        if (slot.used && slot.source_id == source_id && slot.boot_id == boot_id) return &slot;
    }
    return nullptr;
}

Terminal::Slot* Terminal::find_or_alloc(std::uint32_t source_id, std::uint32_t boot_id,
                                        std::uint32_t now_ms) {
    if (Slot* existing = find(source_id, boot_id)) return existing;
    Slot* victim = nullptr;
    for (Slot& slot : slots_) {
        if (!slot.used) {
            victim = &slot;
            break;
        }
        if (victim == nullptr || now_ms - slot.last_used_ms > now_ms - victim->last_used_ms) {
            victim = &slot;
        }
    }
    if (victim->used) ++stats_.origins_evicted;
    ++stats_.origins_seen;
    victim->used = true;
    victim->source_id = source_id;
    victim->boot_id = boot_id;
    victim->last_used_ms = now_ms;
    victim->prompt_painted = false;
    victim->awaiting_result = false;
    victim->result_ready = false;
    victim->async_dirty = false;
    victim->in.clear();
    victim->result.clear();
    victim->async_out.clear();
    victim->queued_line.clear();
    victim->editor.reset();
    victim->editor.setCompletionProvider(completion_);
    return victim;
}

void Terminal::on_input(std::uint32_t source_id, std::uint32_t boot_id, const std::uint8_t* data,
                        std::size_t size, std::uint32_t now_ms) {
    if (data == nullptr || size == 0U) return;
    Slot* slot = find_or_alloc(source_id, boot_id, now_ms);
    slot->last_used_ms = now_ms;
    const std::size_t room = slot->in.size() < kMaxPendingIn ? kMaxPendingIn - slot->in.size() : 0U;
    const std::size_t take = size < room ? size : room;
    slot->in.append(reinterpret_cast<const char*>(data), take);
    stats_.bytes_in_dropped += static_cast<std::uint32_t>(size - take);
}

void Terminal::deliver(std::uint32_t source_id, std::uint32_t boot_id, const char* text,
                       std::uint8_t status) {
    Slot* slot = find(source_id, boot_id);
    if (slot == nullptr) return;
    slot->result = render_command_output(text, status);
    slot->result_ready = true;
}

void Terminal::async_line(const char* text) {
    if (text == nullptr || text[0] == '\0') return;
    std::string line = "! ";
    line += text;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    line += "\r\n";
    for (Slot& slot : slots_) {
        if (!slot.used) continue;
        if (slot.async_out.size() + line.size() <= kMaxAsyncOut) {
            slot.async_out += line;
        } else {
            ++stats_.bytes_in_dropped;  // counted with the other terminal drops
        }
    }
}

void Terminal::submit(Slot& slot, const std::string& line, std::string& out) {
    if (submit_ != nullptr && submit_(context_, slot.source_id, slot.boot_id, line.c_str())) {
        ++stats_.lines_submitted;
        slot.awaiting_result = true;
        return;
    }
    ++stats_.lines_dropped_busy;
    slot.editor.writeResponse("! busy, command dropped", out);
}

void Terminal::pump(std::uint32_t now_ms) {
    for (Slot& slot : slots_) {
        if (!slot.used) continue;
        std::string out;
        if (!slot.prompt_painted) {
            slot.editor.setPrompt(prompt_, out);
            slot.prompt_painted = true;
        }
        if (slot.result_ready) {
            slot.result_ready = false;
            slot.awaiting_result = false;
            slot.editor.writeResponse(slot.result, out);
            slot.result.clear();
        }
        if (!slot.in.empty()) {
            slot.editor.feed(slot.in);
            slot.in.clear();
        }
        // A line typed while the previous command ran goes out now.
        if (!slot.awaiting_result && !slot.queued_line.empty()) {
            std::string next;
            next.swap(slot.queued_line);
            submit(slot, next, out);
        }
        std::string line;
        while (slot.editor.poll(out, &line)) {
            if (slot.awaiting_result) {
                if (slot.queued_line.empty()) {
                    slot.queued_line.swap(line);
                } else {
                    ++stats_.lines_dropped_busy;
                    slot.editor.writeResponse("! busy, command dropped", out);
                }
                continue;
            }
            submit(slot, line, out);
        }
        if (!slot.async_out.empty()) {
            slot.editor.writeAsyncOutput(slot.async_out, out);
            slot.async_out.clear();
            slot.async_dirty = true;
            slot.last_async_ms = now_ms;
        } else if (slot.async_dirty && now_ms - slot.last_async_ms >= kPromptRestoreMs) {
            slot.editor.refreshLine(out);
            slot.async_dirty = false;
        }
        emit(out);
    }
}

void Terminal::emit(const std::string& bytes) {
    if (emit_ == nullptr) return;
    for (std::size_t offset = 0U; offset < bytes.size(); offset += max_chunk_) {
        const std::size_t size = bytes.size() - offset < max_chunk_ ? bytes.size() - offset : max_chunk_;
        if (emit_(context_, reinterpret_cast<const std::uint8_t*>(bytes.data()) + offset, size)) {
            ++stats_.chunks_out;
        } else {
            ++stats_.chunks_out_failed;
        }
    }
}

}  // namespace btpdev
