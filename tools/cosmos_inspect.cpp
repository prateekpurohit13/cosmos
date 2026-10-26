// Interactive single-universe inspector (docs/ui-design.md U3.4).
//
// It runs one universe of the checked-in demo application in this process, captures the versioned
// inspection stream, and pauses at a breakpoint. While paused it prints the read-only state and
// accepts `c` (continue), `s` (step to the next recorded event) or `q` (leave the universe paused
// and exit). Operator commands are scheduled at virtual times and recorded in a command log, so a
// repro is seed + config + commands rather than an unrecorded mutation (D6).
//
//   cosmos_inspect --seed 42 --stop-kind network.recv
//   cosmos_inspect --seed 42 --command crash:0:300000 --continue
//   cosmos_inspect --seed 42 --interactive
//
// The breakpoint matches on the record path and pauses at the next scheduler boundary. That is
// deterministic: no wall clock, no host thread and no event-order rewrite.

#include "demo_scenario.hpp"

#include "cosmos/inspection.hpp"
#include "cosmos/scenario.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    uint64_t seed = 1;
    uint64_t index = 1;
    cosmos::Breakpoint breakpoint{};
    bool has_breakpoint = false;
    bool interactive = false;
    bool continue_after = false;
    bool print_events = true;
    std::string commands_out;
    std::string events_out;
    std::vector<std::string> command_specs;
};

struct CommandSlot {
    cosmos::Simulator* simulator = nullptr;
    cosmos::CommandLog* log = nullptr;
    size_t index = 0;
    bool applied = false;
};

struct WorkloadSlot {
    cosmos::Scenario* scenario = nullptr;
    cosmos::demo::DemoContext* demo = nullptr;
    uint64_t index = 0;
};

void* workload_fiber(void* raw) {
    auto* slot = static_cast<WorkloadSlot*>(raw);
    cosmos::demo::run_demo_workload(*slot->scenario, slot->index, *slot->demo);
    return nullptr;
}

void usage() {
    std::cout
        << "usage: cosmos_inspect [options]\n"
        << "  --seed N              universe seed (decimal or 0x-prefixed)\n"
        << "  --index N             demo universe index (default 1)\n"
        << "  --stop-kind TOKEN     event token from docs/event-stream.md\n"
        << "  --stop-domain NAME    scheduler|network|storage|memory|clock|random|application|"
           "operator|node\n"
        << "  --stop-node N         node id\n"
        << "  --stop-value N        minimum payload.value\n"
        << "  --stop-at NS          minimum virtual time\n"
        << "  --stop-occurrence N   match the Nth event (default 1)\n"
        << "  --command SPEC        crash_node:NODE:AT | reboot_node:NODE:AT | pause_node:NODE:AT\n"
        << "                        resume_node:NODE:AT | partition:A,B:AT | "
           "heal_partition:A,B:AT\n"
        << "  --commands-out PATH   write the recorded command log JSON\n"
        << "  --events-out PATH     write the version-1 event bytes\n"
        << "  --interactive         pause and read c/s/q from stdin\n"
        << "  --continue            drain to quiescence after the breakpoint\n"
        << "  --no-events           do not print the retained event table\n";
}

std::optional<uint64_t> parse_u64(std::string_view text) {
    if (text.empty()) return std::nullopt;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    uint64_t value = 0;
    for (const char raw : text) {
        int digit = -1;
        if (raw >= '0' && raw <= '9')
            digit = raw - '0';
        else if (base == 16 && raw >= 'a' && raw <= 'f')
            digit = raw - 'a' + 10;
        else if (base == 16 && raw >= 'A' && raw <= 'F')
            digit = raw - 'A' + 10;
        else
            return std::nullopt;
        if (value > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(digit)) /
                        static_cast<uint64_t>(base)) {
            return std::nullopt;
        }
        value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
    }
    return value;
}

std::optional<std::vector<uint64_t>> parse_nodes(std::string_view text) {
    std::vector<uint64_t> nodes;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string_view part = comma == std::string_view::npos
                                          ? text.substr(start)
                                          : text.substr(start, comma - start);
        const auto value = parse_u64(part);
        if (!value || *value > std::numeric_limits<cosmos::NodeId>::max()) return std::nullopt;
        nodes.push_back(*value);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return nodes.empty() ? std::nullopt : std::optional<std::vector<uint64_t>>(std::move(nodes));
}

std::vector<std::string_view> split_spec(std::string_view spec) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= spec.size()) {
        const size_t colon = spec.find(':', start);
        parts.push_back(colon == std::string_view::npos ? spec.substr(start)
                                                        : spec.substr(start, colon - start));
        if (colon == std::string_view::npos) break;
        start = colon + 1;
    }
    return parts;
}

bool add_command(cosmos::CommandLog& log, std::string_view spec, std::string& error) {
    const std::vector<std::string_view> parts = split_spec(spec);
    if (parts.size() < 2) {
        error = "command must be KIND:...:AT_NS";
        return false;
    }
    const auto at = parse_u64(parts.back());
    if (!at) {
        error = "command time is not a uint64";
        return false;
    }
    const std::string_view kind = parts.front();
    const cosmos::Time time{static_cast<int64_t>(
        std::min<uint64_t>(*at, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))};
    if (kind == "partition" || kind == "heal_partition") {
        if (parts.size() != 3) {
            error = "partition command must be partition:A,B:AT_NS";
            return false;
        }
        const auto a = parse_nodes(parts[1]);
        if (!a) {
            error = "partition side A is not a node list";
            return false;
        }
        std::vector<cosmos::NodeId> nodes;
        for (const uint64_t node : *a)
            nodes.push_back(static_cast<cosmos::NodeId>(node));
        // The second side is everything not named in A for this demo (one node by default), so a
        // one-sided partition is the common "isolate a node" case rather than an error.
        std::vector<cosmos::NodeId> other;
        for (cosmos::NodeId node = 0; node < 8; ++node) {
            if (std::find(nodes.begin(), nodes.end(), node) == nodes.end()) other.push_back(node);
        }
        log.append(time,
                   kind == "partition" ? cosmos::OperatorCommandKind::Partition
                                       : cosmos::OperatorCommandKind::HealPartition,
                   0, std::move(nodes), std::move(other));
        return true;
    }
    const auto parsed_kind = cosmos::parse_operator_command(kind);
    if (!parsed_kind) {
        error = "unknown command kind \"" + std::string(kind) + "\"";
        return false;
    }
    if (parts.size() != 3) {
        error = "command must be KIND:NODE:AT_NS";
        return false;
    }
    const auto node = parse_u64(parts[1]);
    if (!node || *node > std::numeric_limits<cosmos::NodeId>::max()) {
        error = "command node is not a uint32";
        return false;
    }
    log.append(time, *parsed_kind, static_cast<cosmos::NodeId>(*node));
    return true;
}

void apply_scheduled_command(void* raw) {
    auto* slot = static_cast<CommandSlot*>(raw);
    const cosmos::OperatorCommand& command = slot->log->commands()[slot->index];
    if (cosmos::apply_operator_command(*slot->simulator, command)) {
        slot->applied = true;
        cosmos::record_operator_command(*slot->simulator, command);
    }
}

void print_events(const cosmos::EventSink& sink) {
    std::cout << "retained events: " << sink.size() << ", dropped: " << sink.dropped()
              << ", rejected: " << sink.rejected() << "\n";
    for (size_t index = 0; index < sink.size(); ++index) {
        const cosmos::Event& event = sink[index];
        std::cout << "  #" << event.seq << " t=" << event.at.ns << "ns "
                  << cosmos::name_of(event.kind);
        if (event.node != cosmos::kNoNode) std::cout << " node=" << event.node;
        if (event.task != cosmos::kNoTask) std::cout << " task=" << event.task;
        if (event.correlation_id != cosmos::kNoCorrelation) {
            std::cout << " correlation=" << event.correlation_id;
        }
        std::cout << " subject=" << event.payload.subject << " value=" << event.payload.value
                  << " extra=" << event.payload.extra << " amount_ns=" << event.payload.amount_ns
                  << "\n";
    }
}

bool write_text(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

// Steps to the next accepted event of any kind. The observer counts matches from zero; the next
// event is occurrence `seen + 1` of the match-all predicate.
void step_to_next_event(cosmos::BreakpointContext& context) {
    context.hit = false;
    context.breakpoint = cosmos::Breakpoint{};
    context.breakpoint.occurrence = context.seen + 1;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        const auto take = [&](std::string& value) {
            if (index + 1 >= argc) return false;
            value = argv[++index];
            return true;
        };
        std::string value;
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else if (arg == "--seed" && take(value)) {
            const auto seed = parse_u64(value);
            if (!seed) {
                std::cerr << "cosmos_inspect: invalid --seed\n";
                return 2;
            }
            options.seed = *seed;
        } else if (arg == "--index" && take(value)) {
            const auto universe_index = parse_u64(value);
            if (!universe_index) {
                std::cerr << "cosmos_inspect: invalid --index\n";
                return 2;
            }
            options.index = *universe_index;
        } else if (arg == "--stop-kind" && take(value)) {
            const auto kind = cosmos::parse_event_kind(value);
            if (!kind) {
                std::cerr << "cosmos_inspect: unknown event kind \"" << value << "\"\n";
                return 2;
            }
            options.breakpoint.kind = *kind;
            options.has_breakpoint = true;
        } else if (arg == "--stop-domain" && take(value)) {
            bool found = false;
            for (size_t domain = 0; domain < cosmos::kEventDomainCount; ++domain) {
                const auto candidate = static_cast<cosmos::EventDomain>(domain);
                if (value == cosmos::name_of(candidate)) {
                    options.breakpoint.domain = candidate;
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "cosmos_inspect: unknown event domain \"" << value << "\"\n";
                return 2;
            }
            options.has_breakpoint = true;
        } else if (arg == "--stop-node" && take(value)) {
            const auto node = parse_u64(value);
            if (!node || *node > std::numeric_limits<cosmos::NodeId>::max()) {
                std::cerr << "cosmos_inspect: invalid --stop-node\n";
                return 2;
            }
            options.breakpoint.node = static_cast<cosmos::NodeId>(*node);
            options.has_breakpoint = true;
        } else if (arg == "--stop-value" && take(value)) {
            const auto min_value = parse_u64(value);
            if (!min_value) {
                std::cerr << "cosmos_inspect: invalid --stop-value\n";
                return 2;
            }
            options.breakpoint.min_value = *min_value;
            options.has_breakpoint = true;
        } else if (arg == "--stop-at" && take(value)) {
            const auto at = parse_u64(value);
            if (!at || *at > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                std::cerr << "cosmos_inspect: invalid --stop-at\n";
                return 2;
            }
            options.breakpoint.min_virtual_ns = static_cast<int64_t>(*at);
            options.has_breakpoint = true;
        } else if (arg == "--stop-occurrence" && take(value)) {
            const auto occurrence = parse_u64(value);
            if (!occurrence || *occurrence == 0) {
                std::cerr << "cosmos_inspect: --stop-occurrence must be positive\n";
                return 2;
            }
            options.breakpoint.occurrence = *occurrence;
            options.has_breakpoint = true;
        } else if (arg == "--command" && take(value)) {
            options.command_specs.push_back(value);
        } else if (arg == "--commands-out" && take(value)) {
            options.commands_out = value;
        } else if (arg == "--events-out" && take(value)) {
            options.events_out = value;
        } else if (arg == "--interactive") {
            options.interactive = true;
        } else if (arg == "--continue") {
            options.continue_after = true;
        } else if (arg == "--no-events") {
            options.print_events = false;
        } else {
            std::cerr << "cosmos_inspect: unknown argument \"" << arg << "\"\n";
            usage();
            return 2;
        }
    }

    cosmos::Simulator simulator(options.seed);
    simulator.enable_event_capture(4096);
    cosmos::BreakpointContext breakpoint;
    if (options.has_breakpoint) {
        breakpoint.breakpoint = options.breakpoint;
        simulator.event_sink().set_observer(&cosmos::breakpoint_observer, &breakpoint);
    }

    cosmos::CommandLog log;
    for (const std::string& spec : options.command_specs) {
        std::string error;
        if (!add_command(log, spec, error)) {
            std::cerr << "cosmos_inspect: " << error << "\n";
            return 2;
        }
    }
    std::vector<CommandSlot> slots(log.size());
    for (size_t index = 0; index < log.size(); ++index) {
        slots[index] = CommandSlot{&simulator, &log, index, false};
        if (!simulator.at(log.commands()[index].at, apply_scheduled_command, &slots[index])) {
            std::cerr << "cosmos_inspect: too many scheduled commands\n";
            return 2;
        }
    }

    auto scenario = cosmos::Scenario::on(simulator, cosmos::demo::demo_plan());
    if (!scenario) {
        std::cerr << "cosmos_inspect: the demo plan was rejected\n";
        return 1;
    }
    cosmos::demo::DemoContext demo = cosmos::demo::prepare_demo(*scenario);
    WorkloadSlot workload{&*scenario, &demo, options.index};
    pthread_t workload_thread{};
    if (simulator.spawn_node(0, &workload_thread, workload_fiber, &workload) != 0) {
        std::cerr << "cosmos_inspect: could not spawn the workload fiber\n";
        return 1;
    }

    // The workload runs on its own fiber, so the main fiber stays the drain owner and a
    // breakpoint can genuinely suspend the universe mid-workload instead of waiting for the
    // workload to return.
    auto* previous = cosmos::Simulator::current();
    cosmos::Simulator::set_current(&simulator);
    simulator.scheduler().run_until_quiescence();
    cosmos::Simulator::set_current(previous);

    bool paused = simulator.event_sink().observer_hit();
    if (paused) {
        std::cout << "breakpoint hit after " << breakpoint.seen
                  << " match(es), paused at t=" << simulator.now().ns << "ns\n";
    } else {
        std::cout << "universe drained without a breakpoint hit at t=" << simulator.now().ns
                  << "ns\n";
    }

    if (options.print_events) print_events(simulator.event_sink());
    std::cout << "state:\n";
    cosmos::print_state_json(std::cout, cosmos::capture_state(simulator));
    std::cout << "\n";

    while (paused && options.interactive) {
        std::cout << "[c]ontinue, [s]tep, [q]uit > " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line[0] == 'q') break;
        if (line[0] == 's') {
            step_to_next_event(breakpoint);
        } else if (line[0] == 'c') {
            simulator.event_sink().clear_observer();
        } else {
            continue;
        }
        simulator.event_sink().clear_observer_hit();
        auto* before = cosmos::Simulator::current();
        cosmos::Simulator::set_current(&simulator);
        simulator.scheduler().run_until_quiescence();
        cosmos::Simulator::set_current(before);
        paused = simulator.event_sink().observer_hit();
        if (paused) {
            std::cout << "paused at t=" << simulator.now().ns << "ns\n";
            cosmos::print_state_json(std::cout, cosmos::capture_state(simulator));
            std::cout << "\n";
            std::cout << "\n";
        } else {
            std::cout << "drained at t=" << simulator.now().ns << "ns\n";
        }
    }

    if (paused && (options.continue_after || !options.interactive)) {
        simulator.event_sink().clear_observer();
        simulator.event_sink().clear_observer_hit();
        auto* before = cosmos::Simulator::current();
        cosmos::Simulator::set_current(&simulator);
        simulator.scheduler().run_until_quiescence();
        cosmos::Simulator::set_current(before);
        paused = simulator.event_sink().observer_hit();
    }

    // A drained universe can finish its report. A deliberately paused one must not: the world has
    // not settled, and evaluating a check here would be early rather than merely incomplete.
    if (!paused) {
        cosmos::demo::finish_demo(*scenario, options.index, demo.injected_oom);
    }

    if (!options.commands_out.empty()) {
        std::ostringstream commands;
        cosmos::print_commands_json(commands, options.seed, log);
        if (!write_text(options.commands_out, commands.str())) {
            std::cerr << "cosmos_inspect: could not write " << options.commands_out << "\n";
            return 1;
        }
    }
    if (!options.events_out.empty()) {
        const auto encoded = cosmos::encode_events(simulator.event_sink());
        if (!encoded) {
            std::cerr << "cosmos_inspect: event stream could not be encoded ("
                      << cosmos::name_of(encoded.error()) << ")\n";
            return 1;
        }
        if (!write_text(options.events_out, *encoded)) {
            std::cerr << "cosmos_inspect: could not write " << options.events_out << "\n";
            return 1;
        }
    }
    for (const CommandSlot& slot : slots) {
        if (!slot.applied) {
            std::cerr << "cosmos_inspect: command #" << log.commands()[slot.index].id << " ("
                      << cosmos::name_of(log.commands()[slot.index].kind)
                      << ") was not applied at t=" << log.commands()[slot.index].at.ns
                      << "ns; the universe did not reach that virtual boundary\n";
        }
    }
    return 0;
}
