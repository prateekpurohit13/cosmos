#pragma once

#include "cosmos/faults.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cosmos {

struct NodeInfo {
    NodeId id = 0;
    bool live = true;
};

// Live/crashed state per simulated node. Episodes (P6) and storage durability both read it; who
// may crash a node and when is the harness's decision, not this class's.
class NodeRegistry {
  public:
    explicit NodeRegistry(uint32_t node_count = 1) : crashed_(node_count, false) {}

    uint32_t node_count() const { return static_cast<uint32_t>(crashed_.size()); }
    bool is_known(NodeId id) const { return id < crashed_.size(); }
    bool is_live(NodeId id) const { return is_known(id) && !crashed_[id]; }
    bool is_crashed(NodeId id) const { return is_known(id) && crashed_[id]; }

    // False on an unknown node or one already in that state: a clean refusal, never UB.
    bool crash(NodeId id) {
        if (!is_live(id)) return false;
        crashed_[id] = true;
        ++crashed_count_;
        return true;
    }

    bool reboot(NodeId id) {
        if (!is_crashed(id)) return false;
        crashed_[id] = false;
        --crashed_count_;
        return true;
    }

    uint32_t crashed_count() const { return crashed_count_; }
    uint32_t live_count() const { return node_count() - crashed_count_; }

    // Read-only rows for the node state tab, ordered by node id.
    std::vector<NodeInfo> node_infos() const {
        std::vector<NodeInfo> infos;
        infos.reserve(crashed_.size());
        for (size_t id = 0; id < crashed_.size(); ++id) {
            infos.push_back(NodeInfo{static_cast<NodeId>(id), !crashed_[id]});
        }
        return infos;
    }

    // Growing marks the new nodes live; shrinking refuses when it would forget a crashed one.
    [[nodiscard]] bool set_node_count(uint32_t count) {
        if (count < crashed_.size()) {
            for (size_t i = count; i < crashed_.size(); ++i) {
                if (crashed_[i]) return false;
            }
        }
        crashed_.resize(count, false);
        return true;
    }

  private:
    std::vector<bool> crashed_;
    uint32_t crashed_count_{0};
};

} // namespace cosmos
