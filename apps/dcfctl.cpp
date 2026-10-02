// Data Center Federation - federation membership, delegation and partition reconciliation runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// dcfctl is the operator tool: it speaks the federation protocol as an operator
// peer, which is to say it holds the federation's own authority and needs no
// delegation.
#include <chrono>
#include <iostream>
#include <string>

#include "dcf/cli.hpp"
#include "dcf/json.hpp"
#include "dcf/version.hpp"
#include "dcf/wire.hpp"

namespace {

[[nodiscard]] std::optional<dcf::wire::QueryKind> query_kind(const std::string& verb) {
  if (verb == "version") {
    return dcf::wire::QueryKind::Version;
  }
  if (verb == "status") {
    return dcf::wire::QueryKind::Summary;
  }
  if (verb == "sites") {
    return dcf::wire::QueryKind::Sites;
  }
  if (verb == "site") {
    return dcf::wire::QueryKind::Site;
  }
  if (verb == "delegations") {
    return dcf::wire::QueryKind::Delegations;
  }
  if (verb == "windows") {
    return dcf::wire::QueryKind::Windows;
  }
  if (verb == "reconciliations") {
    return dcf::wire::QueryKind::Reconciliations;
  }
  if (verb == "receipts") {
    return dcf::wire::QueryKind::Receipts;
  }
  if (verb == "recovery") {
    return dcf::wire::QueryKind::Recovery;
  }
  if (verb == "stats") {
    return dcf::wire::QueryKind::Stats;
  }
  return std::nullopt;
}

void usage() {
  std::cout << "dcfctl --endpoint HOST:PORT <verb> [options] [--operation HEX]\n\n"
            << dcf::cli::usage_text();
}

}  // namespace

int main(int argc, char** argv) {
  const dcf::cli::OptionSet parsed = dcf::cli::parse_options(argc, argv, 1);
  if (parsed.has_flag("help") || parsed.positional.empty()) {
    usage();
    return parsed.positional.empty() && !parsed.has_flag("help") ? 2 : 0;
  }
  const auto endpoint_text = parsed.require("endpoint");
  if (!endpoint_text) {
    std::cerr << "error: " << endpoint_text.error().detail << "\n";
    return 2;
  }
  const auto address = dcf::wire::parse_address(endpoint_text.value());
  if (!address) {
    std::cerr << "error: " << address.error().detail << "\n";
    return 2;
  }

  const std::string verb = parsed.positional.front();
  const auto request = dcf::cli::build_request(verb, parsed);
  if (!request) {
    std::cerr << "error: " << request.error().detail << "\n";
    return 2;
  }

  auto connection = dcf::wire::connect(address.value());
  if (!connection) {
    std::cerr << "error: " << connection.error().detail << "\n";
    return 3;
  }
  dcf::wire::Connection link = std::move(connection).value();
  link.set_receive_deadline_ms(30000);

  dcf::wire::Message hello;
  hello.kind = dcf::wire::MessageKind::Hello;
  hello.hello.peer = dcf::wire::PeerKind::Operator;
  hello.hello.protocol_major = dcf::kProtocolVersionMajor;
  hello.hello.protocol_minor = dcf::kProtocolVersionMinor;
  hello.hello.implementation = "dcfctl/" + dcf::version_string();
  if (!link.send(hello, dcf::Limits{})) {
    std::cerr << "error: the federation did not accept the greeting\n";
    return 3;
  }
  const auto ack = link.receive(dcf::Limits{});
  if (!ack || ack.value().kind != dcf::wire::MessageKind::HelloAck ||
      !ack.value().hello_ack.accepted) {
    std::cerr << "error: "
              << (ack.has_value() ? ack.value().hello_ack.detail
                                  : std::string("no greeting answer was received"))
              << "\n";
    return 3;
  }

  if (request.value().query.has_value()) {
    const auto kind = query_kind(request.value().query.value());
    if (!kind.has_value()) {
      std::cerr << "error: '" << verb << "' is not a query this tool has\n";
      return 2;
    }
    dcf::wire::Message query;
    query.kind = dcf::wire::MessageKind::Query;
    query.query.kind = kind.value();
    query.query.argument = request.value().query_argument;
    if (!link.send(query, dcf::Limits{})) {
      std::cerr << "error: the query could not be sent\n";
      return 3;
    }
    const auto result = link.receive(dcf::Limits{});
    if (!result || result.value().kind != dcf::wire::MessageKind::QueryResult) {
      std::cerr << "error: no query result was received\n";
      return 3;
    }
    std::cout << result.value().query_result.body << "\n";
    return result.value().query_result.ok ? 0 : 1;
  }

  dcf::Command command = request.value().command;
  if (const auto operation = parsed.get("operation"); operation.has_value()) {
    const auto id = dcf::parse_identifier<dcf::OperationTag>(operation.value());
    if (!id.has_value()) {
      std::cerr << "error: '" << operation.value() << "' is not an operation identifier\n";
      return 2;
    }
    command.operation = id.value();
  }
  command.origin = dcf::SiteId{};
  command.origin_generation = ack.value().hello_ack.generation;

  dcf::wire::Message message;
  message.kind = dcf::wire::MessageKind::CommandMessage;
  message.command = command;
  if (!link.send(message, dcf::Limits{})) {
    std::cerr << "error: the command could not be sent\n";
    return 3;
  }
  const auto outcome = link.receive(dcf::Limits{});
  if (!outcome || outcome.value().kind != dcf::wire::MessageKind::Outcome) {
    std::cerr << "error: no outcome was received\n";
    return 3;
  }

  const dcf::wire::OutcomeMessage& result = outcome.value().outcome;
  dcf::Json json;
  json.begin_object();
  json.key("verb").value(verb);
  json.key("outcome").value(dcf::to_string(result.code));
  json.key("generation").value(result.generation.value());
  json.key("membership_generation").value(result.membership_generation.value());
  if (!result.reconciliation_outcome.empty()) {
    json.key("reconciliation").value(result.reconciliation_outcome);
    json.key("adopt_generation").value(result.adopt_generation);
    json.key("fenced").value(result.fenced);
  }
  if (!result.detail.empty()) {
    json.key("detail").value(result.detail);
  }
  json.end_object();
  std::cout << json.str() << "\n";
  return result.code == dcf::ErrorCode::None ? 0 : 1;
}
