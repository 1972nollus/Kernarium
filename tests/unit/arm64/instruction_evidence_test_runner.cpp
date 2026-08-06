// SPDX-License-Identifier: GPL-2.0-or-later

#include <cerrno>
#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t child_group = -1;
volatile std::sig_atomic_t forwarded_signal = 0;
std::filesystem::path active_staging_base;

void cleanup_staging_base(const std::filesystem::path &base) noexcept {
  if (base.empty())
    return;
  for (const auto suffix : {std::string_view{}, std::string_view{".raw"},
                            std::string_view{".inputs.jsonl.gz"},
                            std::string_view{".native"}}) {
    const auto member = std::filesystem::path{base.string() +
                                               std::string{suffix}};
    ::unlink((member.string() + ".producer.tmp").c_str());
    ::unlink(member.c_str());
  }
}

[[noreturn]] void fail(const std::string &message) {
  cleanup_staging_base(active_staging_base);
  std::fprintf(stderr, "instruction evidence runner: %s\n", message.c_str());
  std::exit(125);
}

[[nodiscard]] int open_lock(const std::filesystem::path &path, int operation) {
  auto flags = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  if (std::filesystem::is_symlink(path))
    fail("lock is a symlink: " + path.string());
  const auto descriptor = ::open(path.c_str(), flags, 0600);
  if (descriptor < 0)
    fail("cannot open lock: " + path.string());
  struct stat status {};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
    ::close(descriptor);
    fail("lock is not regular: " + path.string());
  }
  if (::flock(descriptor, operation | LOCK_NB) != 0) {
    ::close(descriptor);
    fail("producer/publication lock is active: " + path.string());
  }
  return descriptor;
}

void forward_signal(int signal) {
  forwarded_signal = signal;
  const auto group = static_cast<pid_t>(child_group);
  if (group > 0)
    ::kill(-group, signal);
}

[[nodiscard]] bool safe_producer_name(std::string_view name) {
  if (name.empty())
    return false;
  for (const unsigned char character : name)
    if (!(std::isalnum(character) || character == '_' || character == '-' ||
          character == '.' || character == '+'))
      return false;
  return true;
}

[[nodiscard]] sigset_t supervised_signals() {
  sigset_t signals{};
  sigemptyset(&signals);
  for (const auto signal : {SIGINT, SIGTERM, SIGHUP, SIGQUIT})
    sigaddset(&signals, signal);
  return signals;
}

void install_signal_handlers() {
  struct sigaction action {};
  action.sa_handler = forward_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  for (const auto signal : {SIGINT, SIGTERM, SIGHUP, SIGQUIT})
    if (::sigaction(signal, &action, nullptr) != 0)
      fail("cannot install signal handler");
}

void restore_child_signals(const sigset_t &signals) {
  struct sigaction action {};
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  for (const auto signal : {SIGINT, SIGTERM, SIGHUP, SIGQUIT})
    if (::sigaction(signal, &action, nullptr) != 0)
      ::_exit(126);
  if (::sigprocmask(SIG_UNBLOCK, &signals, nullptr) != 0)
    ::_exit(126);
}

[[nodiscard]] bool group_exists(pid_t group) {
  if (::kill(-group, 0) == 0)
    return true;
  return errno == EPERM;
}

void wait_for_group_exit(pid_t group, unsigned attempts) {
  constexpr timespec delay{.tv_sec = 0, .tv_nsec = 10'000'000};
  for (unsigned attempt = 0; attempt < attempts && group_exists(group);
       ++attempt)
    ::nanosleep(&delay, nullptr);
}

[[nodiscard]] bool terminate_remaining_group(pid_t group) {
  if (!group_exists(group))
    return false;
  ::kill(-group, SIGTERM);
  wait_for_group_exit(group, 100);
  if (!group_exists(group))
    return true;
  ::kill(-group, SIGKILL);
  wait_for_group_exit(group, 100);
  if (group_exists(group))
    fail("child process group did not terminate");
  return true;
}

[[nodiscard]] std::filesystem::path canonical_output(
    const std::filesystem::path &directory, std::string_view filename) {
  if (filename.empty() || filename == "." || filename == ".." ||
      filename.find('/') != std::string_view::npos)
    fail("evidence output basename is invalid");
  return directory / std::string{filename};
}

void remove_stale_staging(const std::filesystem::path &directory,
                          const std::string &producer) {
  const auto prefix = ".producer." + producer + ".runner-stage.";
  std::error_code error;
  for (const auto &entry : std::filesystem::directory_iterator{directory,
                                                                error}) {
    if (error)
      fail("cannot enumerate stale producer staging files");
    if (!entry.path().filename().string().starts_with(prefix))
      continue;
    struct stat status {};
    if (::lstat(entry.path().c_str(), &status) != 0)
      fail("cannot inspect stale producer staging file");
    if (!S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode))
      fail("stale producer staging entry is not removable");
    if (::unlink(entry.path().c_str()) != 0)
      fail("cannot remove stale producer staging file");
  }
  if (error)
    fail("cannot enumerate stale producer staging files");
}

[[nodiscard]] std::filesystem::path make_staging_base(
    const std::filesystem::path &directory, const std::string &producer) {
  remove_stale_staging(directory, producer);
  auto pattern =
      (directory / (".producer." + producer + ".runner-stage.XXXXXX"))
          .string();
  std::string writable = pattern;
  writable.push_back('\0');
  const auto descriptor = ::mkstemp(writable.data());
  if (descriptor < 0)
    fail("cannot reserve producer staging base");
  const std::filesystem::path result{writable.data()};
  if (::close(descriptor) != 0 || ::unlink(result.c_str()) != 0)
    fail("cannot initialize producer staging base");
  return result;
}

[[nodiscard]] bool stage_member_exists(
    const std::filesystem::path &path) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) == 0)
    return true;
  if (errno != ENOENT)
    fail("cannot inspect producer staging member");
  return false;
}

void validate_and_sync_stage_member(const std::filesystem::path &stage,
                                    const std::filesystem::path &final,
                                    std::string_view expected_prefix,
                                    bool gzip_stream) {
  auto flags = O_RDWR | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const auto descriptor = ::open(stage.c_str(), flags);
  if (descriptor < 0)
    fail("cannot open producer staging member");
  struct stat before {};
  struct stat after {};
  if (::lstat(stage.c_str(), &before) != 0 ||
      ::fstat(descriptor, &after) != 0 || !S_ISREG(after.st_mode) ||
      before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
      after.st_size <= 0) {
    ::close(descriptor);
    fail("producer staging member identity is invalid");
  }
  std::array<unsigned char, 64> prefix{};
  const auto wanted = gzip_stream ? 2U : expected_prefix.size();
  if (wanted > prefix.size() ||
      ::pread(descriptor, prefix.data(), wanted, 0) !=
          static_cast<ssize_t>(wanted)) {
    ::close(descriptor);
    fail("producer staging member is truncated");
  }
  if (gzip_stream) {
    if (prefix[0] != 0x1fU || prefix[1] != 0x8bU) {
      ::close(descriptor);
      fail("producer materialization is not a gzip stream");
    }
  } else if (std::string_view{
                 reinterpret_cast<const char *>(prefix.data()), wanted} !=
             expected_prefix) {
    ::close(descriptor);
    fail("producer staging member has an unexpected header");
  }
  struct stat final_status {};
  if (::lstat(final.c_str(), &final_status) == 0) {
    if (!S_ISREG(final_status.st_mode) ||
        ::fchmod(descriptor, final_status.st_mode & 07777) != 0) {
      ::close(descriptor);
      fail("canonical producer output is not replaceable");
    }
  } else if (errno != ENOENT) {
    ::close(descriptor);
    fail("cannot inspect canonical producer output");
  }
  if (::fsync(descriptor) != 0 || ::close(descriptor) != 0)
    fail("cannot sync producer staging member");
}

void publish_or_remove(const std::filesystem::path &stage,
                       const std::filesystem::path &final, bool present) {
  if (present) {
    if (::rename(stage.c_str(), final.c_str()) != 0)
      fail("cannot publish producer staging member");
    return;
  }
  if (::unlink(final.c_str()) != 0 && errno != ENOENT)
    fail("cannot remove stale canonical producer output");
}

void sync_directory(const std::filesystem::path &directory) {
  auto flags = O_RDONLY | O_CLOEXEC;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
  const auto descriptor = ::open(directory.c_str(), flags);
  if (descriptor < 0 || ::fsync(descriptor) != 0) {
    if (descriptor >= 0)
      ::close(descriptor);
    fail("cannot sync producer output directory");
  }
  if (::close(descriptor) != 0)
    fail("cannot close producer output directory");
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2)
    fail("a test command is required");
  const auto *evidence_text = std::getenv("UML_INSTRUCTION_EVIDENCE");
  const auto *producer_text = std::getenv("UML_INSTRUCTION_EVIDENCE_TEST");
  if (evidence_text == nullptr || evidence_text[0] == '\0' ||
      producer_text == nullptr || !safe_producer_name(producer_text))
    fail("evidence path/producer environment is invalid");

  const std::string producer{producer_text};
  const auto evidence_file = std::filesystem::path{evidence_text};
  if (evidence_file.filename() != producer + ".tsv")
    fail("evidence filename is not bound to its producer");
  const auto evidence_directory = evidence_file.parent_path();
  std::error_code error;
  std::filesystem::create_directories(evidence_directory, error);
  if (error)
    fail("evidence directory is invalid");
  const auto canonical_directory =
      std::filesystem::canonical(evidence_directory, error);
  if (error)
    fail("cannot canonicalize evidence directory");
  const auto canonical_evidence =
      canonical_output(canonical_directory, evidence_file.filename().string());
  if (canonical_evidence.string().find('\n') != std::string::npos)
    fail("evidence path contains a marker delimiter");
  const auto gate_path = canonical_directory.parent_path() /
                         (canonical_directory.filename().string() + ".lock");
  const auto producer_path =
      canonical_directory / ("." + producer + ".producer.lock");
  const auto gate = open_lock(gate_path, LOCK_SH);
  const auto producer_lock = open_lock(producer_path, LOCK_EX);
  struct stat gate_status {};
  struct stat producer_status {};
  if (::fstat(gate, &gate_status) != 0 ||
      ::fstat(producer_lock, &producer_status) != 0 ||
      (gate_status.st_dev == producer_status.st_dev &&
       gate_status.st_ino == producer_status.st_ino))
    fail("producer and publication lock identities alias");

  const auto canonical_raw =
      std::filesystem::path{canonical_evidence.string() + ".raw"};
  const auto canonical_materialization = std::filesystem::path{
      canonical_evidence.string() + ".inputs.jsonl.gz"};
  const auto canonical_native =
      std::filesystem::path{canonical_evidence.string() + ".native"};
  const auto *raw_text = std::getenv("UML_INSTRUCTION_RAW_EVIDENCE");
  const bool raw_enabled = raw_text != nullptr && raw_text[0] != '\0' &&
                           raw_text[0] != '0';
  const auto *materialization_text =
      std::getenv("UML_INSTRUCTION_INPUT_MATERIALIZATION");
  const bool materialization_enabled =
      materialization_text != nullptr && materialization_text[0] != '\0' &&
      materialization_text[0] != '0';
  if (materialization_enabled) {
    const auto materialization = std::filesystem::path{materialization_text};
    if (materialization.filename() != producer + ".tsv.inputs.jsonl.gz")
      fail("materialization filename is not bound to its producer");
    const auto materialization_parent =
        std::filesystem::canonical(materialization.parent_path(), error);
    if (error || materialization_parent != canonical_directory)
      fail("materialization parent is not bound to evidence directory");
    const auto requested_materialization = canonical_output(
        canonical_directory, materialization.filename().string());
    if (requested_materialization != canonical_materialization ||
        requested_materialization.string().find('\n') != std::string::npos)
      fail("materialization path contains a marker delimiter");
  }
  active_staging_base = make_staging_base(canonical_directory, producer);
  const auto staging_evidence = active_staging_base;
  const auto staging_raw =
      std::filesystem::path{active_staging_base.string() + ".raw"};
  const auto staging_materialization = std::filesystem::path{
      active_staging_base.string() + ".inputs.jsonl.gz"};
  const auto staging_native =
      std::filesystem::path{active_staging_base.string() + ".native"};
  if (::setenv("UML_INSTRUCTION_EVIDENCE", staging_evidence.c_str(), 1) != 0)
    fail("cannot bind staged evidence output");
  if (materialization_enabled &&
      ::setenv("UML_INSTRUCTION_INPUT_MATERIALIZATION",
               staging_materialization.c_str(), 1) != 0)
    fail("cannot bind staged materialization output");
  const std::string marker =
      "v1\n" + producer + "\n" + staging_evidence.string();
  if (::setenv("UML_INSTRUCTION_EVIDENCE_LOCKED", marker.c_str(), 1) != 0)
    fail("cannot bind evidence lock marker");

  const auto signals = supervised_signals();
  if (::sigprocmask(SIG_BLOCK, &signals, nullptr) != 0)
    fail("cannot block supervised signals");
  install_signal_handlers();

  const auto child = ::fork();
  if (child < 0)
    fail("cannot fork test command");
  if (child == 0) {
    if (::setpgid(0, 0) != 0)
      ::_exit(126);
    restore_child_signals(signals);
    ::execvp(argv[1], &argv[1]);
    std::fprintf(stderr, "cannot execute unit test: %s\n", argv[1]);
    ::_exit(127);
  }
  if (::setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH)
    fail("cannot create child process group");
  child_group = child;
  if (::sigprocmask(SIG_UNBLOCK, &signals, nullptr) != 0)
    fail("cannot unblock supervised signals");

  int status = 0;
  bool terminating = false;
  bool killed = false;
  auto deadline = std::chrono::steady_clock::time_point::max();
  for (;;) {
    const auto observed = ::waitpid(child, &status, WNOHANG);
    if (observed == child)
      break;
    if (observed < 0 && errno != EINTR)
      fail("cannot wait for unit test");
    const auto now = std::chrono::steady_clock::now();
    if (forwarded_signal != 0 && !terminating) {
      terminating = true;
      deadline = now + std::chrono::seconds{1};
    } else if (terminating && !killed && now >= deadline) {
      ::kill(-child, SIGKILL);
      killed = true;
      deadline = now + std::chrono::seconds{1};
    } else if (killed && now >= deadline) {
      fail("cannot reap terminated unit test process group leader");
    }
    constexpr timespec delay{.tv_sec = 0, .tv_nsec = 10'000'000};
    ::nanosleep(&delay, nullptr);
  }
  if (::sigprocmask(SIG_BLOCK, &signals, nullptr) != 0)
    fail("cannot block signals after producer exit");
  sigset_t pending_signals {};
  if (::sigpending(&pending_signals) != 0)
    fail("cannot inspect pending producer signals");
  if (forwarded_signal == 0)
    for (const auto signal : {SIGINT, SIGTERM, SIGHUP, SIGQUIT})
      if (sigismember(&pending_signals, signal) == 1) {
        forwarded_signal = signal;
        break;
      }
  const bool had_remaining_group = terminate_remaining_group(child);
  child_group = -1;
  const bool successful = forwarded_signal == 0 && !had_remaining_group &&
                          WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (successful) {
    const bool has_evidence = stage_member_exists(staging_evidence);
    const bool has_raw = stage_member_exists(staging_raw);
    const bool has_materialization =
        stage_member_exists(staging_materialization);
    const bool has_native = stage_member_exists(staging_native);
    if (stage_member_exists(
            std::filesystem::path{staging_evidence.string() +
                                  ".producer.tmp"}) ||
        stage_member_exists(
            std::filesystem::path{staging_raw.string() + ".producer.tmp"}) ||
        stage_member_exists(std::filesystem::path{
            staging_materialization.string() + ".producer.tmp"}) ||
        stage_member_exists(
            std::filesystem::path{staging_native.string() + ".producer.tmp"}))
      fail("successful producer left an incomplete staging member");
    const bool has_standard = has_evidence || has_raw || has_materialization;
    if ((has_standard &&
         (!has_evidence || (raw_enabled && !has_raw) ||
          (materialization_enabled && !has_materialization))) ||
        (!raw_enabled && has_raw) ||
        (!materialization_enabled && has_materialization))
      fail("successful producer output set is incomplete");
    if (has_evidence)
      validate_and_sync_stage_member(staging_evidence, canonical_evidence,
                                     "stable_id\tsection\t", false);
    if (has_raw)
      validate_and_sync_stage_member(staging_raw, canonical_raw,
                                     "ctest\tcase\t", false);
    if (has_materialization)
      validate_and_sync_stage_member(staging_materialization,
                                     canonical_materialization, {}, true);
    if (has_native)
      validate_and_sync_stage_member(staging_native, canonical_native,
                                     "oracle_case_id\tctest\t", false);

    publish_or_remove(staging_native, canonical_native, has_native);
    publish_or_remove(staging_materialization, canonical_materialization,
                      has_materialization);
    publish_or_remove(staging_raw, canonical_raw, has_raw);
    publish_or_remove(staging_evidence, canonical_evidence, has_evidence);
    sync_directory(canonical_directory);
  }
  cleanup_staging_base(active_staging_base);
  active_staging_base.clear();
  ::close(producer_lock);
  ::close(gate);
  if (had_remaining_group)
    return 125;
  if (forwarded_signal != 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
    return 128 + forwarded_signal;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  if (forwarded_signal != 0)
    return 128 + forwarded_signal;
  return 125;
}
