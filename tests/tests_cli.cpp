// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Integration obligations for the inspection tool: ctopctl must exercise the
// real library end to end, must report the documented exit codes, must be
// byte-deterministic for identical input, and every structural answer must end
// with the claim-boundary statement.
//
// The whole file is conditional on COOLING_TOPOLOGY_CLI_PATH, so the suite still
// builds when the tool is not part of the configuration.
//
// The child-process runner below is local to this translation unit on purpose.
// The CLI is launched with its output captured through pipes (never a console
// that could raise an interactive error dialog) and the pipe is drained to end
// of file before the child is waited for, with no deadline anywhere: a large
// answer can therefore never deadlock against a full pipe, and a hang is a
// defect to diagnose rather than something bounded away. A configuration that
// names a tool which is not there skips with a note instead of failing.

#include "dccp/cooling_topology/version.hpp"
#include "store_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(COOLING_TOPOLOGY_CLI_PATH)

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace ct = dccp::cooling_topology;

const char* kCliPath = COOLING_TOPOLOGY_CLI_PATH;

/// The eleven claims the library never makes; the posture report has one line
/// for each of them.
constexpr std::size_t kExcludedClaimCount = 11;

struct CliRun {
  int exit_code = 0;
  /// Merged stdout and stderr, in the order the child wrote them.
  std::string output;
};

/// Abandons the current test with a message, like CT_REQUIRE but readable for
/// process and file failures.
void require(bool condition, std::string_view what) {
  if (!condition) {
    const std::string text(what);
    ::ct_test::report_failure(__FILE__, __LINE__, text, "required");
    throw ::ct_test::TestAborted{text};
  }
}

bool contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

std::size_t line_count(const std::string& text) {
  return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

/// The last line of an answer, without its terminator.
std::string last_line(const std::string& text) {
  std::size_t end = text.size();
  while (end != 0 && (text[end - 1] == '\n' || text[end - 1] == '\r')) {
    --end;
  }
  if (end == 0) {
    return std::string();
  }
  const std::size_t newline = text.rfind('\n', end - 1);
  const std::size_t start = newline == std::string::npos ? 0 : newline + 1;
  return text.substr(start, end - start);
}

/// Every structural answer ends with the claim-boundary statement.
bool ends_with_boundary(const std::string& output) {
  return last_line(output) == "posture: " + std::string(ct::posture_statement());
}

/// The tool is launched by the path the build system configured. When that path
/// names no file, every case below skips with a note: a tool that was configured
/// but not built is a configuration fact, not a defect in the library.
bool cli_available() {
  std::error_code error;
  const bool present = std::filesystem::is_regular_file(kCliPath, error) && !error;
  if (!present) {
    ::ct_test::report_note(std::string("the inspection tool is not present at ") + kCliPath + ": case skipped");
  }
  return present;
}

#if defined(_WIN32)

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int length = static_cast<int>(text.size());
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
  if (size <= 0) {
    return std::wstring();
  }
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, out.data(), size);
  return out;
}

/// Quotes one argument the way the C runtime parses a command line back, so a
/// path containing spaces or quotes survives unchanged.
std::string quote_argument(const std::string& argument) {
  std::string out = "\"";
  std::size_t backslashes = 0;
  for (const char byte : argument) {
    if (byte == '\\') {
      ++backslashes;
      continue;
    }
    if (byte == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, '\\');
    backslashes = 0;
    out.push_back(byte);
  }
  out.append(backslashes * 2, '\\');
  out.push_back('"');
  return out;
}

/// Runs the tool with output captured through an anonymous pipe and blocks
/// until it exits. There is no timeout: the child either exits or the test run
/// stops, which is the intended behaviour.
CliRun run_cli(const std::vector<std::string>& arguments) {
  CliRun run;
  std::string command_line = quote_argument(kCliPath);
  for (const std::string& argument : arguments) {
    command_line.push_back(' ');
    command_line += quote_argument(argument);
  }
  std::wstring wide_command = widen(command_line);

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  require(::CreatePipe(&read_end, &write_end, &attributes, 0) != FALSE, "CreatePipe failed");
  ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  HANDLE input_device =
      ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (input_device == INVALID_HANDLE_VALUE) {
    ::CloseHandle(read_end);
    ::CloseHandle(write_end);
    require(false, "the NUL input device could not be opened");
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(STARTUPINFOW);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input_device;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;

  PROCESS_INFORMATION process{};
  // CREATE_NO_WINDOW: the child never receives a console, so no interactive
  // error-reporting dialog can appear; everything it writes arrives on the pipe.
  const BOOL started = ::CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
  ::CloseHandle(write_end);
  ::CloseHandle(input_device);
  if (started == FALSE) {
    ::CloseHandle(read_end);
    require(false, "the inspection tool could not be started at " + std::string(kCliPath));
  }

  std::string output;
  std::array<char, 4096> buffer{};
  DWORD read = 0;
  while (::ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != FALSE &&
         read != 0) {
    output.append(buffer.data(), static_cast<std::size_t>(read));
  }
  ::CloseHandle(read_end);

  ::WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0;
  const BOOL has_exit_code = ::GetExitCodeProcess(process.hProcess, &exit_code);
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  require(has_exit_code != FALSE, "the exit code of the inspection tool could not be read");

  run.exit_code = static_cast<int>(exit_code);
  run.output = std::move(output);
  return run;
}

#else

CliRun run_cli(const std::vector<std::string>& arguments) {
  CliRun run;
  int pipe_ends[2] = {-1, -1};
  require(::pipe(pipe_ends) == 0, "pipe failed");

  std::vector<std::string> storage;
  storage.reserve(arguments.size() + 1);
  storage.emplace_back(kCliPath);
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& token : storage) {
    argv.push_back(token.data());
  }
  argv.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) {
    ::close(pipe_ends[0]);
    ::close(pipe_ends[1]);
    require(false, "fork failed");
  }
  if (child == 0) {
    ::dup2(pipe_ends[1], STDOUT_FILENO);
    ::dup2(pipe_ends[1], STDERR_FILENO);
    ::close(pipe_ends[0]);
    ::close(pipe_ends[1]);
    ::execv(kCliPath, argv.data());
    ::_exit(127);
  }
  ::close(pipe_ends[1]);

  std::string output;
  std::array<char, 4096> buffer{};
  ssize_t count = 0;
  while ((count = ::read(pipe_ends[0], buffer.data(), buffer.size())) > 0) {
    output.append(buffer.data(), static_cast<std::size_t>(count));
  }
  ::close(pipe_ends[0]);

  int status = 0;
  require(::waitpid(child, &status, 0) == child, "waitpid failed");
  run.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  run.output = std::move(output);
  return run;
}

#endif

/// The export of a generation as an import document: the tool consumes exactly
/// the grammar the library writes, so the fixtures below are real documents.
std::string exported_document(const ct::TopologyDraft& draft) {
  const auto topology = ct::Topology::create_first(draft);
  CT_REQUIRE(topology.has_value());
  return ct::export_import(topology.value());
}

/// Creates a store in the given directory and publishes one generation, using
/// only the public library API, so the tool has a real store to inspect. Returns
/// the empty string on success and the failing step otherwise.
std::string publish_first_generation(const std::string& root, const ct::TopologyDraft& draft) {
  ct::StoreOptions options;
  options.root = root;
  options.mode = ct::StoreMode::ReadWrite;
  options.create_if_missing = true;
  options.retained_generations = 2;
  options.idempotency_retention = 4;
  const auto store_id = ct::StoreId::parse("store.cli-probe");
  if (!store_id.has_value()) {
    return "the store identity is not valid";
  }
  // A store is bound to the facility it holds, and it refuses a generation that
  // describes another one, so the binding is taken from the draft itself.
  auto created = ct::Store::create(options, store_id.value(), draft.facility);
  if (!created.has_value()) {
    return "Store::create: " + created.error().to_string();
  }
  ct::Store store = std::move(created.value());
  const auto info = store.info();
  if (!info.has_value()) {
    return "Store::info: " + info.error().to_string();
  }
  const auto mutation = ct::MutationId::parse("m.cli-probe.1");
  const auto ordinal = ct::AttemptOrdinal::parse(1);
  if (!mutation.has_value() || !ordinal.has_value()) {
    return "the mutation identity or attempt ordinal is not valid";
  }
  ct::MutationAuthority authority;
  authority.epoch = info.value().epoch;
  authority.incarnation = info.value().incarnation;
  authority.expected_base = info.value().head;
  ct::PublicationRequest request;
  request.authority = authority;
  request.mutation = mutation.value();
  request.attempt = ordinal.value();
  request.draft = draft;
  const auto receipt = store.publish(request);
  if (!receipt.has_value()) {
    return "Store::publish: " + receipt.error().to_string();
  }
  const auto closed = store.close();
  if (!closed.has_value()) {
    return "Store::close: " + closed.error().to_string();
  }
  return std::string();
}

/// A document that parses but is structurally invalid: the edge names a node
/// that does not exist in the generation.
constexpr std::string_view kInvalidDocument =
    "facility facility:dc-cli@4\n"
    "provenance producer=\"ctopctl-test\" origin=authored witness=\"integration\"\n"
    "node source-fw cooling_source kind=facility_water\n"
    "edge e-missing supplies source-fw.supply_out -> ghost.source_in\n";

// ---------------------------------------------------------------------------
// Usage and preamble
// ---------------------------------------------------------------------------

CT_TEST(cli_usage_paths_are_usage_errors) {
  if (!cli_available()) {
    return;
  }
  const CliRun no_command = run_cli({});
  CT_CHECK_EQ(no_command.exit_code, 2);
  CT_CHECK(contains(no_command.output, "no command given"));
  CT_CHECK(contains(no_command.output, "usage: ctopctl"));

  // The tool takes no global options, and no spelling of "help" is special: the
  // usage path is what answers here, through the exit code.
  const CliRun help = run_cli({"--help"});
  CT_CHECK_EQ(help.exit_code, 2);
  CT_CHECK(contains(help.output, "unknown option"));
  CT_CHECK(contains(help.output, "boundary: "));

  const CliRun short_help = run_cli({"-h"});
  CT_CHECK_EQ(short_help.exit_code, 2);
  CT_CHECK(contains(short_help.output, "unknown option"));

  const CliRun unknown_option = run_cli({"--not-a-flag"});
  CT_CHECK_EQ(unknown_option.exit_code, 2);
  CT_CHECK(contains(unknown_option.output, "unknown option"));

  const CliRun unknown_command = run_cli({"definitely-not-a-verb"});
  CT_CHECK_EQ(unknown_command.exit_code, 2);
  CT_CHECK(contains(unknown_command.output, "unknown command"));

  const CliRun too_many_arguments = run_cli({"version", "--not-a-flag"});
  CT_CHECK_EQ(too_many_arguments.exit_code, 2);
  CT_CHECK(contains(too_many_arguments.output, "version takes no arguments"));
}

CT_TEST(cli_version_and_posture_state_the_claim_boundary) {
  if (!cli_available()) {
    return;
  }
  const CliRun version = run_cli({"version"});
  CT_CHECK_EQ(version.exit_code, 0);
  CT_CHECK(contains(version.output, std::string(ct::version_string())));
  CT_CHECK(contains(version.output, std::string(ct::component_id())));
  CT_CHECK(contains(version.output, std::string(ct::systems_boundary())));
  CT_CHECK(contains(version.output, "boundary: " + std::string(ct::systems_boundary())));
  CT_CHECK(contains(version.output, "posture: " + std::string(ct::posture_statement())));

  // One line per excluded claim, each reported as not owned, plus the header.
  const CliRun posture = run_cli({"posture"});
  CT_CHECK_EQ(posture.exit_code, 0);
  CT_CHECK_EQ(line_count(posture.output), kExcludedClaimCount + 1);
  for (std::size_t index = 0; index < kExcludedClaimCount; ++index) {
    const ct::ExcludedClaim claim = static_cast<ct::ExcludedClaim>(index);
    const std::string line = "  not_owned  " + std::string(ct::to_token(claim)) +
                             "  owner: " + std::string(ct::excluded_claim_owner(claim));
    CT_CHECK_MSG(contains(posture.output, line), line);
  }
  CT_CHECK(contains(posture.output, "claim boundary of dccp-cooling-topology"));
}

// ---------------------------------------------------------------------------
// Validation, export and round trip
// ---------------------------------------------------------------------------

CT_TEST(cli_validate_accepts_a_valid_document_and_names_the_primary_error) {
  if (!cli_available()) {
    return;
  }
  ct_test::ScratchDir scratch("cli-validate");
  const std::string valid = scratch.child("reference.ctg");
  CT_REQUIRE(ct_test::write_text_file(valid, exported_document(ct_test::reference_facility())));

  const CliRun accepted = run_cli({"validate", valid});
  CT_CHECK_EQ(accepted.exit_code, 0);
  CT_CHECK(contains(accepted.output, "valid=true"));
  CT_CHECK(contains(accepted.output, "errors=0"));
  CT_CHECK(ends_with_boundary(accepted.output));

  const std::string invalid = scratch.child("invalid.ctg");
  CT_REQUIRE(ct_test::write_text_file(invalid, kInvalidDocument));
  const CliRun rejected = run_cli({"validate", invalid});
  CT_CHECK_EQ(rejected.exit_code, 1);
  CT_CHECK(contains(rejected.output, "endpoint_missing"));
  CT_CHECK(contains(rejected.output, "valid=false"));
  CT_CHECK(ends_with_boundary(rejected.output));

  // A document that cannot even be parsed is rejected the same way, with the
  // stable code rather than a message the caller would have to match.
  const std::string unparsable = scratch.child("unparsable.ctg");
  CT_REQUIRE(ct_test::write_text_file(unparsable, std::string("facility facility:dc-cli@4\nnode n-1 bogus_kind\n")));
  const CliRun parse_rejected = run_cli({"validate", unparsable});
  CT_CHECK_EQ(parse_rejected.exit_code, 1);
  CT_CHECK(contains(parse_rejected.output, "unknown_enum_token"));
}

CT_TEST(cli_export_reimports_to_the_same_digest) {
  if (!cli_available()) {
    return;
  }
  ct_test::ScratchDir scratch("cli-export");
  const ct::TopologyDraft draft = ct_test::reference_facility();
  const std::string document = scratch.child("reference.ctg");
  CT_REQUIRE(ct_test::write_text_file(document, exported_document(draft)));

  const CliRun exported = run_cli({"export", document});
  CT_CHECK_EQ(exported.exit_code, 0);
  CT_CHECK(contains(exported.output, "facility "));
  CT_CHECK(contains(exported.output, "provenance "));

  // The exported document re-imports in-process to the same generation: same
  // digest, same tables. This is the export/import fixed point the store and
  // diff diagnostics depend on.
  const auto reparsed = ct::parse_import(exported.output);
  CT_REQUIRE(reparsed.has_value());
  const auto rebuilt = ct::Topology::create_first(reparsed.value());
  CT_REQUIRE(rebuilt.has_value());
  const ct::Topology original = ct_test::build(draft);
  CT_CHECK_EQ(rebuilt->digest().to_hex(), original.digest().to_hex());
  CT_CHECK_EQ(rebuilt->node_count(), original.node_count());
  CT_CHECK_EQ(rebuilt->edge_count(), original.edge_count());
  CT_CHECK_EQ(rebuilt->group_count(), original.group_count());
  const auto rebuilt_bytes = rebuilt->canonical_bytes();
  const auto original_bytes = original.canonical_bytes();
  CT_REQUIRE(rebuilt_bytes.has_value() && original_bytes.has_value());
  CT_CHECK_EQ(rebuilt_bytes.value(), original_bytes.value());

  const CliRun again = run_cli({"export", document});
  CT_CHECK_EQ(again.exit_code, 0);
  CT_CHECK_EQ(again.output, exported.output);

  // The exported grammar is never an authority: the tool refuses to read its own
  // inspection rendering back.
  const CliRun shown = run_cli({"show", document});
  CT_CHECK_EQ(shown.exit_code, 0);
  const std::string rendering = scratch.child("rendering.ctg");
  CT_REQUIRE(ct_test::write_text_file(rendering, shown.output));
  const CliRun rendering_is_not_input = run_cli({"validate", rendering});
  CT_CHECK_EQ(rendering_is_not_input.exit_code, 1);
}

// ---------------------------------------------------------------------------
// Structural answers
// ---------------------------------------------------------------------------

CT_TEST(cli_structural_answers_are_deterministic_and_end_with_the_boundary) {
  if (!cli_available()) {
    return;
  }
  ct_test::ScratchDir scratch("cli-answers");
  const std::string document = scratch.child("reference.ctg");
  CT_REQUIRE(ct_test::write_text_file(document, exported_document(ct_test::reference_facility())));

  const std::vector<std::vector<std::string>> commands = {
      {"sources", document},
      {"upstream", document, "sink:rack-a1"},
      {"downstream", document, "source:facility-water"},
      {"paths", document, "source:facility-water", "sink:rack-a1"},
      {"circuits", document, "source:facility-water", "sink:rack-a1"},
      {"independent", document, "source:facility-water", "sink:rack-a1"},
      {"spof", document, "sink:rack-a1"},
      {"components", document},
      {"sink", document, "sink:rack-a1"},
      {"zone", document, "zone:cold-aisle-1"},
      {"blast", document, "loop:secondary"},
      {"group", document, "group:pumps"},
      {"membership", document, "pump:p-1"},
  };
  for (const std::vector<std::string>& command : commands) {
    const CliRun first = run_cli(command);
    const CliRun second = run_cli(command);
    CT_CHECK_MSG(first.exit_code == 0, command.front() + " exited " + std::to_string(first.exit_code) + ": " +
                                            first.output.substr(0, 256));
    CT_CHECK_MSG(first.output == second.output, command.front() + " is not byte-deterministic");
    CT_CHECK_MSG(ends_with_boundary(first.output), command.front() + " does not end with the claim boundary");
  }

  // Spot checks that each answer is about the fixture and not about nothing.
  CT_CHECK(contains(run_cli({"sources", document}).output, "source:facility-water"));
  CT_CHECK(contains(run_cli({"upstream", document, "sink:rack-a1"}).output, "source:facility-water"));
  CT_CHECK(contains(run_cli({"downstream", document, "source:facility-water"}).output, "sink:rack-a1"));
  CT_CHECK(contains(run_cli({"paths", document, "source:facility-water", "sink:rack-a1"}).output,
                    "claim=structurally_possible"));
  CT_CHECK(contains(run_cli({"independent", document, "source:facility-water", "sink:rack-a1"}).output,
                    "edge_disjoint_paths="));
  CT_CHECK(contains(run_cli({"components", document}).output, "components count="));
  CT_CHECK(contains(run_cli({"group", document, "group:pumps"}).output, "group:pumps"));
  CT_CHECK(contains(run_cli({"membership", document, "pump:p-1"}).output, "group:pumps"));
}

CT_TEST(cli_unknown_node_is_a_stable_structural_rejection) {
  if (!cli_available()) {
    return;
  }
  ct_test::ScratchDir scratch("cli-unknown-node");
  const std::string document = scratch.child("reference.ctg");
  CT_REQUIRE(ct_test::write_text_file(document, exported_document(ct_test::reference_facility())));

  const CliRun unknown = run_cli({"upstream", document, "ghost-node"});
  CT_CHECK_EQ(unknown.exit_code, 1);
  CT_CHECK(contains(unknown.output, "not_found"));
  CT_CHECK(contains(unknown.output, "ghost-node"));
  CT_CHECK(contains(unknown.output, "boundary: "));

  // The same question twice is refused the same way.
  const CliRun unknown_again = run_cli({"upstream", document, "ghost-node"});
  CT_CHECK_EQ(unknown_again.output, unknown.output);

  // A spelling that is not a canonical identifier is a usage error, not a
  // structural rejection: the two channels carry different exit codes.
  const CliRun malformed = run_cli({"upstream", document, "node/1"});
  CT_CHECK_EQ(malformed.exit_code, 2);
  CT_CHECK(contains(malformed.output, "invalid_argument"));

  const CliRun missing_file = run_cli({"show", scratch.child("absent.ctg")});
  CT_CHECK_EQ(missing_file.exit_code, 2);
  CT_CHECK(contains(missing_file.output, "input file cannot be opened"));

  const CliRun missing_argument = run_cli({"paths", document, "source:facility-water"});
  CT_CHECK_EQ(missing_argument.exit_code, 2);
  CT_CHECK(contains(missing_argument.output, "requires <from> <to> node arguments"));
}

CT_TEST(cli_diff_reports_the_change_lines_of_two_generations) {
  if (!cli_available()) {
    return;
  }
  ct_test::ScratchDir scratch("cli-diff");
  ct::TopologyDraft after = ct_test::reference_facility();
  after.nodes.push_back(ct_test::crah_node("crah:extra"));
  const std::string before_file = scratch.child("before.ctg");
  const std::string after_file = scratch.child("after.ctg");
  CT_REQUIRE(ct_test::write_text_file(before_file, exported_document(ct_test::reference_facility())));
  CT_REQUIRE(ct_test::write_text_file(after_file, exported_document(after)));

  const CliRun changed = run_cli({"diff", before_file, after_file});
  CT_CHECK_EQ(changed.exit_code, 0);
  CT_CHECK(contains(changed.output, "before_generation=1"));
  CT_CHECK(contains(changed.output, "after_generation=2"));
  CT_CHECK(contains(changed.output, "entries="));
  CT_CHECK(contains(changed.output, "node_delta=1"));
  CT_CHECK(contains(changed.output, "edge_delta=0"));
  CT_CHECK(contains(changed.output, "node_added crah:extra"));
  CT_CHECK(ends_with_boundary(changed.output));

  // Two generations of the same content differ in binding, not in structure.
  const CliRun unchanged = run_cli({"diff", before_file, before_file});
  CT_CHECK_EQ(unchanged.exit_code, 0);
  CT_CHECK(contains(unchanged.output, "entries=0"));
  CT_CHECK(contains(unchanged.output, "node_delta=0"));
  CT_CHECK(contains(unchanged.output, "edge_delta=0"));

  // The removal direction reports the same element from the other side.
  const CliRun reversed = run_cli({"diff", after_file, before_file});
  CT_CHECK_EQ(reversed.exit_code, 0);
  CT_CHECK(contains(reversed.output, "node_removed crah:extra"));
  CT_CHECK(contains(reversed.output, "node_delta=-1"));
}

// ---------------------------------------------------------------------------
// Durable store inspection
// ---------------------------------------------------------------------------

CT_TEST(cli_store_inspection_uses_the_documented_exit_codes) {
  if (!cli_available()) {
    return;
  }
  ct_test::ScratchDir scratch("cli-store");
  const std::string root = scratch.child("site");
  const std::string setup_failure = publish_first_generation(root, ct_test::reference_facility());
  if (!setup_failure.empty()) {
    ct_test::report_note("store setup failed: " + setup_failure);
  }
  CT_REQUIRE(setup_failure.empty());

  const CliRun verify = run_cli({"store", root, "verify"});
  CT_CHECK_EQ(verify.exit_code, 0);
  CT_CHECK(contains(verify.output, "ok=true"));
  CT_CHECK(contains(verify.output, "verified head=true"));
  CT_CHECK(contains(verify.output, "chain=true"));
  CT_CHECK(contains(verify.output, "generations_verified=1"));
  CT_CHECK(ends_with_boundary(verify.output));

  const CliRun verify_again = run_cli({"store", root, "verify"});
  CT_CHECK_EQ(verify_again.output, verify.output);

  const ct::Topology published = ct_test::build(ct_test::reference_facility());
  const CliRun head = run_cli({"store", root, "head"});
  CT_CHECK_EQ(head.exit_code, 0);
  CT_CHECK(contains(head.output, "generation=1"));
  CT_CHECK(contains(head.output, "nodes=" + std::to_string(published.node_count())));
  CT_CHECK(contains(head.output, "edges=" + std::to_string(published.edge_count())));
  CT_CHECK(ends_with_boundary(head.output));

  const CliRun history = run_cli({"store", root, "history"});
  CT_CHECK_EQ(history.exit_code, 0);
  CT_CHECK(contains(history.output, "chain_verified=true"));
  CT_CHECK(ends_with_boundary(history.output));

  // A directory that holds no store is a persistence failure, not a structural
  // one, and it is reported with the store error code.
  const CliRun absent = run_cli({"store", scratch.child("absent-store"), "verify"});
  CT_CHECK_EQ(absent.exit_code, 3);
  CT_CHECK(contains(absent.output, "store_not_found"));
  CT_CHECK(contains(absent.output, "boundary: "));

  // An unknown inspection subcommand is a usage error.
  const CliRun unknown_subcommand = run_cli({"store", root, "bogus"});
  CT_CHECK_EQ(unknown_subcommand.exit_code, 2);
  CT_CHECK(contains(unknown_subcommand.output, "store subcommand must be"));
}

}  // namespace

#else

CT_TEST(cli_tool_not_configured) {
  // The suite was configured without the inspection tool; there is nothing to
  // prove about a tool that was not built.
  ct_test::report_note("COOLING_TOPOLOGY_CLI_PATH is not defined: the ctopctl integration suite is skipped");
  CT_CHECK(true);
}

#endif
