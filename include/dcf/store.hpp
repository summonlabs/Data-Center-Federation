// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dcf/platform.hpp"
#include "dcf/state.hpp"
#include "dcf/types.hpp"

namespace dcf {

// On-disk format identifiers. The version is part of the file, so a store
// written by a later build is refused with a reason instead of being
// misinterpreted.
inline constexpr char kJournalMagic[8] = {'D', 'C', 'F', 'L', 'O', 'G', '0', '1'};
inline constexpr char kSnapshotMagic[8] = {'D', 'C', 'F', 'S', 'N', 'A', 'P', '1'};
inline constexpr std::uint32_t kStoreFormatVersion = 1;

struct RecoveryReport {
  // True when the store already existed and was reopened.
  bool reopened{false};
  // Bytes removed from the tail of a journal segment because the record there
  // was incomplete. A torn tail is the last record only, and it is reported
  // rather than silently dropped.
  std::uint64_t torn_tail_bytes_removed{0};
  std::uint64_t journal_segments_read{0};
  std::uint64_t entries_replayed{0};
  std::uint64_t snapshot_sequence{0};
  std::uint64_t snapshot_generation{0};
  std::vector<std::string> orphaned_files_removed{};
  Digest state_digest{};
};

// ---------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------
// A single-writer, crash-safe store built from one immutable snapshot plus one
// append-only journal segment, described by a CURRENT file that is replaced
// atomically.
//
// The commit boundary is flush() on the journal segment: once commit() returns,
// the entry survives process death and machine restart. The publish boundary is
// separate and is owned by the caller: a committed entry is not visible to
// readers until the engine has applied it.
class JournalStore {
 public:
  JournalStore() = default;
  JournalStore(const JournalStore&) = delete;
  JournalStore& operator=(const JournalStore&) = delete;
  JournalStore(JournalStore&& other) noexcept;
  JournalStore& operator=(JournalStore&& other) noexcept;
  ~JournalStore();

  // Opens, recovers, and repairs only the tail. Interior corruption is refused
  // and never truncated through.
  [[nodiscard]] static Result<JournalStore> open(const std::string& root, const Limits& limits);

  // Makes an entry durable. Returns only after the bytes have reached storage.
  [[nodiscard]] Result<Ack> commit(const JournalEntry& entry);
  // Writes a snapshot of the committed state and retires superseded files. The
  // snapshot covers exactly the state that was committed when it was taken, so
  // it can never supersede an entry that has not been written yet.
  [[nodiscard]] Result<Ack> compact(const FederationState& state);
  [[nodiscard]] Result<Ack> close();

  [[nodiscard]] bool is_open() const noexcept { return open_; }
  [[nodiscard]] const std::string& root() const noexcept { return root_; }
  [[nodiscard]] const Limits& limits() const noexcept { return limits_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] const FederationState& recovered_state() const noexcept { return recovered_; }
  [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] std::uint64_t durable_bytes() const noexcept;

 private:
  [[nodiscard]] Result<Ack> load_current();
  [[nodiscard]] Result<Ack> write_current(std::uint64_t snapshot_sequence,
                                          const std::string& snapshot_name,
                                          const std::string& journal_name);
  [[nodiscard]] Result<Ack> load_snapshot(const std::string& name);
  [[nodiscard]] Result<Ack> replay_journal(const std::string& name);
  [[nodiscard]] Result<Ack> remove_orphans(const std::string& snapshot_name,
                                           const std::string& journal_name);

  [[nodiscard]] std::string path_of(const std::string& name) const;

  std::string root_{};
  Limits limits_{};
  platform::FileLock lock_{};
  platform::AppendFile journal_{};
  RecoveryReport recovery_{};
  FederationState recovered_{};
  std::string snapshot_name_{};
  std::string journal_name_{};
  std::uint64_t snapshot_sequence_{0};
  std::uint64_t last_sequence_{0};
  bool open_{false};
};

}  // namespace dcf
