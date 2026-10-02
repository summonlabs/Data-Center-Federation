// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "dcf/store.hpp"

#include <algorithm>
#include <cstring>

#include "dcf/hash.hpp"

namespace dcf {
namespace {

constexpr std::size_t kJournalHeaderBytes = 8 + 4 + 8;
constexpr std::size_t kSnapshotHeaderBytes = 8 + 4 + 8 + 8 + 4 + 32;
constexpr const char* kCurrentName = "CURRENT";
constexpr const char* kLockName = "store.lock";
constexpr const char* kSnapshotPrefix = "state.";
constexpr const char* kSnapshotSuffix = ".dcfsnap";
constexpr const char* kJournalPrefix = "journal.";
constexpr const char* kJournalSuffix = ".dcflog";

[[nodiscard]] std::string name_for(const char* prefix, std::uint64_t number, const char* suffix) {
  return std::string(prefix) + std::to_string(number) + suffix;
}

[[nodiscard]] bool has_suffix(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

[[nodiscard]] bool is_snapshot_name(const std::string& name) {
  return name.rfind(kSnapshotPrefix, 0) == 0 && has_suffix(name, kSnapshotSuffix);
}

[[nodiscard]] bool is_journal_name(const std::string& name) {
  return name.rfind(kJournalPrefix, 0) == 0 && has_suffix(name, kJournalSuffix);
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[index]) << (static_cast<unsigned>(index) * 8U);
  }
  return value;
}

[[nodiscard]] std::uint64_t read_u64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (static_cast<unsigned>(index) * 8U);
  }
  return value;
}

[[nodiscard]] std::string parse_line(const std::string& text, std::size_t& offset) {
  const std::size_t end = text.find('\n', offset);
  const std::size_t stop = end == std::string::npos ? text.size() : end;
  std::string line = text.substr(offset, stop - offset);
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  offset = end == std::string::npos ? text.size() : end + 1;
  return line;
}

[[nodiscard]] Result<std::uint64_t> parse_u64(const std::string& text, const char* what) {
  if (text.empty()) {
    return make_error(ErrorCode::Corruption, std::string(what) + " is empty");
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return make_error(ErrorCode::Corruption,
                        std::string(what) + " is not a decimal number: '" +
                            sanitize_for_terminal(text) + "'");
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      return make_error(ErrorCode::Corruption, std::string(what) + " overflows");
    }
    value = value * 10U + digit;
  }
  return value;
}

}  // namespace

JournalStore::JournalStore(JournalStore&& other) noexcept
    : root_(std::move(other.root_)),
      limits_(other.limits_),
      lock_(std::move(other.lock_)),
      journal_(std::move(other.journal_)),
      recovery_(std::move(other.recovery_)),
      recovered_(std::move(other.recovered_)),
      snapshot_name_(std::move(other.snapshot_name_)),
      journal_name_(std::move(other.journal_name_)),
      snapshot_sequence_(other.snapshot_sequence_),
      last_sequence_(other.last_sequence_),
      open_(other.open_) {
  // The moved-from store must not close the journal segment the new owner now
  // holds, so it stops claiming to be open.
  other.open_ = false;
}

JournalStore& JournalStore::operator=(JournalStore&& other) noexcept {
  if (this != &other) {
    if (open_) {
      static_cast<void>(close());
    }
    root_ = std::move(other.root_);
    limits_ = other.limits_;
    lock_ = std::move(other.lock_);
    journal_ = std::move(other.journal_);
    recovery_ = std::move(other.recovery_);
    recovered_ = std::move(other.recovered_);
    snapshot_name_ = std::move(other.snapshot_name_);
    journal_name_ = std::move(other.journal_name_);
    snapshot_sequence_ = other.snapshot_sequence_;
    last_sequence_ = other.last_sequence_;
    open_ = other.open_;
    other.open_ = false;
  }
  return *this;
}

JournalStore::~JournalStore() {
  if (open_) {
    // Destruction must not throw and must not lose a durable report, so the
    // failure of a close during unwinding is reported by the next open instead.
    static_cast<void>(close());
  }
}

std::string JournalStore::path_of(const std::string& name) const {
  if (root_.empty()) {
    return name;
  }
  const char last = root_.back();
  if (last == '/' || last == '\\') {
    return root_ + name;
  }
  return root_ + "/" + name;
}

std::uint64_t JournalStore::durable_bytes() const noexcept {
  return journal_.is_open() ? journal_.size() : 0;
}

Result<JournalStore> JournalStore::open(const std::string& root, const Limits& limits) {
  if (root.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a store root path is required");
  }
  const auto created = platform::create_directories(root);
  if (!created) {
    return created.error();
  }
  if (!platform::is_writable_directory(root)) {
    return make_error(ErrorCode::Io,
                      "store root '" + sanitize_for_terminal(root) + "' is not writable");
  }

  JournalStore store;
  store.root_ = root;
  store.limits_ = limits;

  auto lock = platform::FileLock::acquire(store.path_of(kLockName));
  if (!lock) {
    return lock.error();
  }
  store.lock_ = std::move(lock).value();

  // A temporary file left behind by a crash during an atomic replace is never
  // authoritative, so it is removed before anything else is read.
  const auto entries = platform::list_directory(root);
  if (!entries) {
    return entries.error();
  }
  for (const std::string& name : entries.value()) {
    if (has_suffix(name, ".tmp")) {
      static_cast<void>(platform::remove_file(store.path_of(name)));
    }
  }

  const auto loaded = store.load_current();
  if (!loaded) {
    return loaded.error();
  }
  const auto replayed = store.replay_journal(store.journal_name_);
  if (!replayed) {
    return replayed.error();
  }
  const auto cleaned = store.remove_orphans(store.snapshot_name_, store.journal_name_);
  if (!cleaned) {
    return cleaned.error();
  }

  auto journal = platform::AppendFile::open(store.path_of(store.journal_name_));
  if (!journal) {
    return journal.error();
  }
  store.journal_ = std::move(journal).value();
  store.recovery_.state_digest = store.recovered_.canonical_digest();
  store.open_ = true;
  return store;
}

Result<Ack> JournalStore::load_current() {
  const std::string current_path = path_of(kCurrentName);
  const auto present = platform::exists(current_path);
  if (!present) {
    return present.error();
  }

  if (!present.value()) {
    // A brand new store. The first journal segment is created immediately so
    // that a later open finds exactly what this one wrote.
    journal_name_ = name_for(kJournalPrefix, 1, kJournalSuffix);
    snapshot_name_.clear();
    snapshot_sequence_ = 0;
    last_sequence_ = 0;

    std::vector<std::uint8_t> header;
    header.insert(header.end(), kJournalMagic, kJournalMagic + sizeof(kJournalMagic));
    put_u32(header, kStoreFormatVersion);
    put_u64(header, 1);

    auto segment = platform::AppendFile::open(path_of(journal_name_));
    if (!segment) {
      return segment.error();
    }
    auto opened = std::move(segment).value();
    const auto written = opened.append(header);
    if (!written) {
      return written.error();
    }
    const auto flushed = opened.flush();
    if (!flushed) {
      return flushed.error();
    }
    const auto closed = opened.close();
    if (!closed) {
      return closed.error();
    }
    const auto described = write_current(0, std::string{}, journal_name_);
    if (!described) {
      return described.error();
    }
    recovery_.reopened = false;
    return Ack{};
  }

  const auto raw = platform::read_file(current_path, 4096);
  if (!raw) {
    return raw.error();
  }
  const std::string text(raw.value().begin(), raw.value().end());
  std::size_t offset = 0;
  const std::string snapshot_sequence_text = parse_line(text, offset);
  const std::string snapshot_name = parse_line(text, offset);
  const std::string journal_name = parse_line(text, offset);
  const std::string extra = parse_line(text, offset);

  if (!extra.empty()) {
    return make_error(ErrorCode::Corruption, "CURRENT carries more lines than the format defines");
  }
  if (journal_name.empty() || !is_journal_name(journal_name)) {
    return make_error(ErrorCode::Corruption,
                      "CURRENT names '" + sanitize_for_terminal(journal_name) +
                          "' which is not a journal segment name");
  }

  const auto sequence = parse_u64(snapshot_sequence_text, "CURRENT snapshot sequence");
  if (!sequence) {
    return sequence.error();
  }
  snapshot_sequence_ = sequence.value();
  last_sequence_ = snapshot_sequence_;

  if (snapshot_name == "-" || snapshot_name.empty()) {
    snapshot_name_.clear();
    if (snapshot_sequence_ != 0) {
      return make_error(ErrorCode::Corruption,
                        "CURRENT declares snapshot sequence " +
                            std::to_string(snapshot_sequence_) +
                            " but names no snapshot file");
    }
  } else {
    if (!is_snapshot_name(snapshot_name)) {
      return make_error(ErrorCode::Corruption,
                        "CURRENT names '" + sanitize_for_terminal(snapshot_name) +
                            "' which is not a snapshot name");
    }
    const auto present_snapshot = platform::exists(path_of(snapshot_name));
    if (!present_snapshot) {
      return present_snapshot.error();
    }
    if (!present_snapshot.value()) {
      return make_error(ErrorCode::Corruption,
                        "the snapshot '" + sanitize_for_terminal(snapshot_name) +
                            "' named by CURRENT does not exist; the store cannot be opened "
                            "without inventing the state it described");
    }
    snapshot_name_ = snapshot_name;
    const auto loaded = load_snapshot(snapshot_name_);
    if (!loaded) {
      return loaded.error();
    }
  }

  const auto present_journal = platform::exists(path_of(journal_name));
  if (!present_journal) {
    return present_journal.error();
  }
  if (!present_journal.value()) {
    return make_error(ErrorCode::Corruption,
                      "the journal segment '" + sanitize_for_terminal(journal_name) +
                          "' named by CURRENT does not exist");
  }
  journal_name_ = journal_name;
  recovery_.reopened = true;
  return Ack{};
}

Result<Ack> JournalStore::write_current(std::uint64_t snapshot_sequence,
                                        const std::string& snapshot_name,
                                        const std::string& journal_name) {
  std::string text;
  text.append(std::to_string(snapshot_sequence));
  text.push_back('\n');
  text.append(snapshot_name.empty() ? "-" : snapshot_name);
  text.push_back('\n');
  text.append(journal_name);
  text.push_back('\n');
  const std::span<const std::uint8_t> bytes(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  return platform::write_file_atomically(path_of(kCurrentName), bytes);
}

Result<Ack> JournalStore::load_snapshot(const std::string& name) {
  const auto raw = platform::read_file(path_of(name), limits_.max_frame_bytes + kSnapshotHeaderBytes);
  if (!raw) {
    return raw.error();
  }
  const std::vector<std::uint8_t>& bytes = raw.value();
  if (bytes.size() < kSnapshotHeaderBytes) {
    return make_error(ErrorCode::Corruption,
                      "snapshot '" + sanitize_for_terminal(name) + "' is shorter than its header");
  }
  if (std::memcmp(bytes.data(), kSnapshotMagic, sizeof(kSnapshotMagic)) != 0) {
    return make_error(ErrorCode::Corruption,
                      "snapshot '" + sanitize_for_terminal(name) + "' has the wrong magic");
  }
  const std::uint32_t version = read_u32(bytes.data() + 8);
  if (version != kStoreFormatVersion) {
    return make_error(ErrorCode::Unsupported,
                      "snapshot '" + sanitize_for_terminal(name) + "' is format version " +
                          std::to_string(version) + " and this build reads version " +
                          std::to_string(kStoreFormatVersion));
  }
  const std::uint64_t sequence = read_u64(bytes.data() + 12);
  const std::uint64_t payload_length = read_u64(bytes.data() + 20);
  const std::uint32_t payload_crc = read_u32(bytes.data() + 28);
  Digest stored_digest;
  for (std::size_t index = 0; index < stored_digest.bytes.size(); ++index) {
    stored_digest.bytes[index] = bytes[32 + index];
  }

  const std::uint64_t available = static_cast<std::uint64_t>(bytes.size() - kSnapshotHeaderBytes);
  if (payload_length != available) {
    return make_error(ErrorCode::Corruption,
                      "snapshot '" + sanitize_for_terminal(name) + "' declares " +
                          std::to_string(payload_length) + " payload bytes and holds " +
                          std::to_string(available));
  }
  if (payload_length > limits_.max_frame_bytes) {
    return make_error(ErrorCode::BoundsExceeded,
                      "snapshot payload of " + std::to_string(payload_length) +
                          " bytes exceeds the limit of " +
                          std::to_string(limits_.max_frame_bytes));
  }
  const std::span<const std::uint8_t> payload(bytes.data() + kSnapshotHeaderBytes,
                                              static_cast<std::size_t>(payload_length));
  if (crc32c(payload) != payload_crc) {
    return make_error(ErrorCode::Corruption,
                      "snapshot '" + sanitize_for_terminal(name) +
                          "' fails its checksum; the file was damaged in place");
  }

  Decoder decoder(payload, limits_);
  auto state = FederationState::decode_from(decoder, limits_);
  if (!state) {
    return make_error(state.error().code,
                      "snapshot '" + sanitize_for_terminal(name) + "' does not decode: " +
                          state.error().detail);
  }
  const auto trailing = decoder.require_end();
  if (!trailing) {
    return make_error(ErrorCode::Corruption,
                      "snapshot '" + sanitize_for_terminal(name) + "' has trailing bytes");
  }
  if (state.value().sequence().value() != sequence) {
    return make_error(ErrorCode::Corruption,
                      "snapshot '" + sanitize_for_terminal(name) + "' declares sequence " +
                          std::to_string(sequence) + " and encodes sequence " +
                          std::to_string(state.value().sequence().value()));
  }
  if (state.value().canonical_digest() != stored_digest) {
    return make_error(ErrorCode::Corruption,
                      "snapshot '" + sanitize_for_terminal(name) +
                          "' does not match its recorded digest");
  }

  recovered_ = std::move(state).value();
  recovery_.snapshot_sequence = sequence;
  recovery_.snapshot_generation = recovered_.generation().value();
  return Ack{};
}

Result<Ack> JournalStore::replay_journal(const std::string& name) {
  const std::string path = path_of(name);
  const auto raw = platform::read_file(path, limits_.max_frame_bytes * 64ULL);
  if (!raw) {
    return raw.error();
  }
  const std::vector<std::uint8_t>& bytes = raw.value();
  if (bytes.size() < kJournalHeaderBytes) {
    return make_error(ErrorCode::Corruption,
                      "journal segment '" + sanitize_for_terminal(name) +
                          "' is shorter than its header");
  }
  if (std::memcmp(bytes.data(), kJournalMagic, sizeof(kJournalMagic)) != 0) {
    return make_error(ErrorCode::Corruption,
                      "journal segment '" + sanitize_for_terminal(name) + "' has the wrong magic");
  }
  const std::uint32_t version = read_u32(bytes.data() + 8);
  if (version != kStoreFormatVersion) {
    return make_error(ErrorCode::Unsupported,
                      "journal segment '" + sanitize_for_terminal(name) +
                          "' is format version " + std::to_string(version) +
                          " and this build reads version " +
                          std::to_string(kStoreFormatVersion));
  }

  ++recovery_.journal_segments_read;
  std::size_t offset = kJournalHeaderBytes;
  std::size_t good_end = offset;
  bool torn = false;

  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    if (remaining < 8) {
      torn = true;
      break;
    }
    // A filesystem that extends a file with zeroes on crash leaves a run of
    // zeroes where a record would have been. That is an unwritten region, not a
    // corrupt record, and it is treated as a torn tail.
    const bool all_zero = std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                      bytes.end(),
                                      [](std::uint8_t byte) { return byte == 0U; });
    if (all_zero) {
      torn = true;
      break;
    }
    const std::uint32_t length = read_u32(bytes.data() + offset);
    const std::uint32_t expected_crc = read_u32(bytes.data() + offset + 4);
    if (static_cast<std::uint64_t>(length) > limits_.max_frame_bytes) {
      return make_error(ErrorCode::InteriorCorruption,
                        "journal segment '" + sanitize_for_terminal(name) + "' declares a record of " +
                            std::to_string(length) + " bytes at offset " +
                            std::to_string(offset) + " which exceeds the limit of " +
                            std::to_string(limits_.max_frame_bytes));
    }
    if (remaining < 8U + static_cast<std::size_t>(length)) {
      // The record is incomplete: this is the classic torn tail, and it is the
      // only case in which bytes are removed.
      torn = true;
      break;
    }
    const std::span<const std::uint8_t> payload(bytes.data() + offset + 8, length);
    if (crc32c(payload) != expected_crc) {
      return make_error(ErrorCode::InteriorCorruption,
                        "journal segment '" + sanitize_for_terminal(name) +
                            "' has a record at offset " + std::to_string(offset) +
                            " whose checksum does not match; the segment is damaged in place and "
                            "will not be truncated through");
    }
    Decoder decoder(payload, limits_);
    auto entry = decode_journal_entry(decoder);
    if (!entry) {
      return make_error(ErrorCode::InteriorCorruption,
                        "journal segment '" + sanitize_for_terminal(name) +
                            "' has a record at offset " + std::to_string(offset) +
                            " that does not decode: " + entry.error().detail);
    }
    const auto trailing = decoder.require_end();
    if (!trailing) {
      return make_error(ErrorCode::InteriorCorruption,
                        "journal segment '" + sanitize_for_terminal(name) +
                            "' has a record at offset " + std::to_string(offset) +
                            " with trailing bytes");
    }
    const JournalEntry value = std::move(entry).value();
    if (value.sequence.value() <= snapshot_sequence_) {
      // Already covered by the snapshot. It is not applied twice.
      offset += 8U + length;
      good_end = offset;
      continue;
    }
    const auto applied = recovered_.apply(value, limits_);
    if (!applied) {
      return make_error(applied.error().code,
                        "journal segment '" + sanitize_for_terminal(name) +
                            "' has a record at offset " + std::to_string(offset) +
                            " that the state machine refused: " + applied.error().detail);
    }
    ++recovery_.entries_replayed;
    last_sequence_ = value.sequence.value();
    offset += 8U + length;
    good_end = offset;
  }

  if (torn) {
    const std::uint64_t removed = static_cast<std::uint64_t>(bytes.size() - good_end);
    recovery_.torn_tail_bytes_removed += removed;
    auto segment = platform::AppendFile::open(path);
    if (!segment) {
      return segment.error();
    }
    auto opened = std::move(segment).value();
    const auto truncated = opened.truncate_to(good_end);
    if (!truncated) {
      return truncated.error();
    }
  }

  if (last_sequence_ < snapshot_sequence_) {
    last_sequence_ = snapshot_sequence_;
  }
  return Ack{};
}

Result<Ack> JournalStore::remove_orphans(const std::string& snapshot_name,
                                         const std::string& journal_name) {
  const auto entries = platform::list_directory(root_);
  if (!entries) {
    return entries.error();
  }
  for (const std::string& name : entries.value()) {
    if (name == kCurrentName || name == kLockName) {
      continue;
    }
    if (name == snapshot_name || name == journal_name) {
      continue;
    }
    if (is_snapshot_name(name) || is_journal_name(name) || has_suffix(name, ".tmp")) {
      const auto removed = platform::remove_file(path_of(name));
      if (!removed) {
        return removed.error();
      }
      recovery_.orphaned_files_removed.push_back(name);
    }
  }
  std::sort(recovery_.orphaned_files_removed.begin(), recovery_.orphaned_files_removed.end());
  return Ack{};
}

Result<Ack> JournalStore::commit(const JournalEntry& entry) {
  if (!open_) {
    return make_error(ErrorCode::Closed, "the store is not open");
  }
  if (entry.sequence.value() != last_sequence_ + 1) {
    return make_error(ErrorCode::Conflict,
                      "entry sequence " + std::to_string(entry.sequence.value()) +
                          " does not follow the durable sequence " +
                          std::to_string(last_sequence_));
  }

  Encoder encoder;
  encode(encoder, entry);
  const std::span<const std::uint8_t> payload = encoder.view();
  if (payload.size() > limits_.max_frame_bytes) {
    return make_error(ErrorCode::BoundsExceeded,
                      "the encoded entry is " + std::to_string(payload.size()) +
                          " bytes which exceeds the limit of " +
                          std::to_string(limits_.max_frame_bytes));
  }

  std::vector<std::uint8_t> frame;
  frame.reserve(8 + payload.size());
  put_u32(frame, static_cast<std::uint32_t>(payload.size()));
  put_u32(frame, crc32c(payload));
  frame.insert(frame.end(), payload.begin(), payload.end());

  const auto appended = journal_.append(frame);
  if (!appended) {
    return appended.error();
  }
  // The durability boundary: nothing above this line is visible to anyone until
  // the bytes are on storage.
  const auto flushed = journal_.flush();
  if (!flushed) {
    return flushed.error();
  }
  last_sequence_ = entry.sequence.value();
  return Ack{};
}

Result<Ack> JournalStore::compact(const FederationState& state) {
  if (!open_) {
    return make_error(ErrorCode::Closed, "the store is not open");
  }
  if (state.sequence().value() != last_sequence_) {
    return make_error(ErrorCode::Conflict,
                      "compaction was asked to snapshot sequence " +
                          std::to_string(state.sequence().value()) +
                          " and the durable sequence is " + std::to_string(last_sequence_));
  }

  const std::uint64_t sequence = state.sequence().value();
  const std::string new_snapshot = name_for(kSnapshotPrefix, sequence, kSnapshotSuffix);
  const std::string new_journal = name_for(kJournalPrefix, sequence + 1, kJournalSuffix);

  Encoder payload_encoder;
  state.encode_into(payload_encoder);
  const std::span<const std::uint8_t> payload = payload_encoder.view();
  if (payload.size() > limits_.max_frame_bytes) {
    return make_error(ErrorCode::BoundsExceeded,
                      "the encoded snapshot payload is " + std::to_string(payload.size()) +
                          " bytes which exceeds the limit of " +
                          std::to_string(limits_.max_frame_bytes));
  }
  const Digest digest = state.canonical_digest();

  std::vector<std::uint8_t> file;
  file.reserve(kSnapshotHeaderBytes + payload.size());
  file.insert(file.end(), kSnapshotMagic, kSnapshotMagic + sizeof(kSnapshotMagic));
  put_u32(file, kStoreFormatVersion);
  put_u64(file, sequence);
  put_u64(file, static_cast<std::uint64_t>(payload.size()));
  put_u32(file, crc32c(payload));
  file.insert(file.end(), digest.bytes.begin(), digest.bytes.end());
  file.insert(file.end(), payload.begin(), payload.end());

  // 1. The snapshot must be durable before anything points at it.
  const auto written = platform::write_file_atomically(path_of(new_snapshot), file);
  if (!written) {
    return written.error();
  }

  // 2. The new journal segment must exist and be durable, empty, and carry the
  //    right first sequence, before CURRENT names it.
  {
    std::vector<std::uint8_t> header;
    header.insert(header.end(), kJournalMagic, kJournalMagic + sizeof(kJournalMagic));
    put_u32(header, kStoreFormatVersion);
    put_u64(header, sequence + 1);
    auto segment = platform::AppendFile::open(path_of(new_journal));
    if (!segment) {
      return segment.error();
    }
    auto opened = std::move(segment).value();
    const auto appended = opened.append(header);
    if (!appended) {
      return appended.error();
    }
    const auto flushed = opened.flush();
    if (!flushed) {
      return flushed.error();
    }
    const auto closed = opened.close();
    if (!closed) {
      return closed.error();
    }
  }

  // 3. Publish the new pair. Until this replace completes, the previous pair is
  //    the truth and the new files are orphans.
  const auto published = write_current(sequence, new_snapshot, new_journal);
  if (!published) {
    return published.error();
  }

  // 4. Only now is it safe to retire the superseded files.
  const auto closed = journal_.close();
  if (!closed) {
    return closed.error();
  }
  auto segment = platform::AppendFile::open(path_of(new_journal));
  if (!segment) {
    return segment.error();
  }
  journal_ = std::move(segment).value();

  const std::string old_snapshot = snapshot_name_;
  const std::string old_journal = journal_name_;
  snapshot_name_ = new_snapshot;
  journal_name_ = new_journal;
  snapshot_sequence_ = sequence;

  if (!old_snapshot.empty() && old_snapshot != new_snapshot) {
    static_cast<void>(platform::remove_file(path_of(old_snapshot)));
  }
  if (!old_journal.empty() && old_journal != new_journal) {
    static_cast<void>(platform::remove_file(path_of(old_journal)));
  }
  return Ack{};
}

Result<Ack> JournalStore::close() {
  if (!open_) {
    return Ack{};
  }
  const auto flushed = journal_.flush();
  const auto closed = journal_.close();
  lock_.release();
  open_ = false;
  if (!flushed) {
    return flushed.error();
  }
  return closed;
}

}  // namespace dcf
