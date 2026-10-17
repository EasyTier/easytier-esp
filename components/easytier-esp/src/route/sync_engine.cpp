/**
 * @file sync_engine.cpp
 * @brief 路由同步引擎实现（拓扑合并、自身信息、请求/响应与 dhcp）。
 */
#include "route/sync_engine.hpp"

#include <cstring>

#include "support/log.hpp"
#include "support/random.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.SYNC";

}  // namespace

/** @brief 配置引擎并复位状态。 */
void SyncEngine::configure(const Settings& settings, uint32_t my_peer_id) noexcept {
    // 只拷贝引擎真正需要的字段：入口列表可能很大，且引擎根本用不到它。
    config_ = EngineConfig{};
    config_.instance_id = settings.instance_id;
    config_.hostname = settings.hostname;
    config_.has_static_ipv4 = settings.has_static_ipv4;
    config_.static_ipv4 = settings.static_ipv4;
    config_.static_prefix = settings.static_prefix;
    config_.dhcp = settings.dhcp;
    my_peer_id_ = my_peer_id;
    reset();
    my_ipv4_ = config_.has_static_ipv4 ? config_.static_ipv4 : 0;
    my_prefix_ = config_.has_static_ipv4 ? config_.static_prefix : 24;
    dhcp_.configure(settings.dhcp_has_subnet, settings.dhcp_subnet, settings.dhcp_prefix);
    dhcp_.reset();
}

/** @brief 复位会话相关状态（保留配置与自身地址）。 */
void SyncEngine::reset() noexcept {
    topology_.reset();
    active_peer_count_ = 0;
    active_peers_.fill(ActivePeer{});
    default_session_id_ = 0;
    self_info_version_ = 1;
    conn_row_version_ = 1;
    self_peer_info_len_ = 0;
    direct_peer_count_ = 0;
    table_.recompute(topology_, my_peer_id_);
}

/**
 * @brief 登记握手成功。
 * @details 第一个对端握手时初始化会话（自身信息、邻接行版本）；后续对端只是加入
 *          直连集合，不重置拓扑——否则每新增一个入口都会丢掉已学到的路由。
 */
void SyncEngine::on_peer_up(uint32_t remote_peer_id, uint64_t session_id,
                           uint32_t now) noexcept {
    const bool first = (active_peer_count_ == 0);
    size_t index = kMaxActivePeers;
    for (size_t i = 0; i < active_peer_count_; ++i) {
        if (active_peers_[i].peer_id == remote_peer_id) {
            index = i;
            break;
        }
    }
    const uint64_t effective_session = session_id != 0 ? session_id : 1;
    if (index == kMaxActivePeers) {
        if (active_peer_count_ < active_peers_.size()) {
            index = active_peer_count_++;
            active_peers_[index] = ActivePeer{remote_peer_id, effective_session};
        }
    } else if (active_peers_[index].session_id != 0 &&
               active_peers_[index].session_id != effective_session) {
        // 上游语义：session id 变化说明对端重建了路由会话，此前保存的信息不再可信，
        // 必须清空并要求重新同步（否则会拿旧拓扑继续转发）。
        ET_LOGW(kTag, "peer %u changed session id, clearing its saved route info",
                static_cast<unsigned>(remote_peer_id));
        topology_.reset_peer(remote_peer_id);
        active_peers_[index].session_id = effective_session;
        ++self_info_version_;
    } else {
        active_peers_[index].session_id = effective_session;
    }
    if (default_session_id_ == 0) {
        default_session_id_ = effective_session;
    }
    if (!first) {
        return;  // 已有会话：仅加入直连集合
    }
    self_info_version_ = 1;
    conn_row_version_ = 1;
    topology_.reset();
    // 自身先进入拓扑（最短路起点），再构造要宣告的信息。
    topology_.set_self(my_peer_id_, my_ipv4_ != 0, my_ipv4_, now);
    if (!build_self_info(now)) {
        ET_LOGE(kTag, "cannot build self route peer info");
    }
}

/** @brief 某直连对端断开：还有其它直连时保留拓扑，全断才清空。 */
void SyncEngine::on_peer_down(uint32_t remote_peer_id) noexcept {
    size_t write = 0;
    for (size_t i = 0; i < active_peer_count_; ++i) {
        if (active_peers_[i].peer_id != remote_peer_id) {
            active_peers_[write++] = active_peers_[i];
        }
    }
    active_peer_count_ = write;
    if (active_peer_count_ == 0) {
        // 没有任何直连：拓扑不再可信，立即清空并撤销所有路由。
        topology_.reset();
        table_.recompute(topology_, my_peer_id_);
    }
}

/** @brief 查询某对端的会话 id（未知返回 0）。 */
uint64_t SyncEngine::session_id_of(uint32_t peer_id) const noexcept {
    for (size_t i = 0; i < active_peer_count_; ++i) {
        if (active_peers_[i].peer_id == peer_id) {
            return active_peers_[i].session_id;
        }
    }
    return 0;
}

/** @brief 设置直连对端集合。 */
void SyncEngine::set_direct_peers(const uint32_t* peers, size_t count, uint32_t now) noexcept {
    direct_peer_count_ = 0;
    for (size_t i = 0; i < count && i < direct_peers_.size(); ++i) {
        if (peers[i] != kInvalidPeerId && peers[i] != my_peer_id_) {
            direct_peers_[direct_peer_count_++] = peers[i];
        }
    }
    // 邻接行内容变了：版本 +1，让对端识别为更新。
    ++conn_row_version_;
    // 把自身邻接行并入本地拓扑：本节点在图上必须有出边，否则无法把流量交给直连对端
    // （上游的 conn_map 同样包含自身行）。这里用真实时间戳，避免被过期清理误删。
    (void)topology_.merge_conn_row(my_peer_id_, conn_row_version_, direct_peers_.data(),
                                   direct_peer_count_, now);
}

/** @brief 构造本节点 RoutePeerInfo 编码。 */
bool SyncEngine::build_self_info(uint32_t now) noexcept {
    if (self_peer_route_id_ == 0) {
        self_peer_route_id_ = random_u64();
        if (self_peer_route_id_ == 0) {
            self_peer_route_id_ = 1;
        }
    }
    SelfRouteInfo info{};
    info.peer_id = my_peer_id_;
    std::memcpy(&info.inst_id, config_.instance_id.data(), sizeof(info.inst_id));
    info.peer_route_id = self_peer_route_id_;
    info.version = self_info_version_;
    info.has_ipv4 = my_ipv4_ != 0;
    info.ipv4 = my_ipv4_;
    info.network_length = my_prefix_;
    // 当前能力：支持邻接收敛、禁用 P2P；不宣告 public server / avoid relay。
    info.feature_flags = peer_feature::SupportConnListSync | peer_feature::DisableP2p;
    info.hostname = config_.hostname;
    info.last_update_secs = now / 1000;  // 仅参考；对端会用本地时间覆盖
    const size_t len = encode_route_peer_info(info, self_peer_info_);
    if (len == 0) {
        return false;
    }
    self_peer_info_len_ = static_cast<uint16_t>(len);
    return true;
}

/** @brief 收集拓扑中通告的已用地址（不含自身）。 */
void SyncEngine::collect_used_addresses() noexcept {
    dhcp_used_ = Ipv4UseSet{};
    for (const TopoPeer& peer : topology_.peers()) {
        if (!peer.has_ipv4 || peer.peer_id == my_peer_id_) {
            continue;
        }
        dhcp_used_.add(peer.ipv4, peer.network_length);
    }
}

/** @brief dhcp 自分配：地址变化时刷新自身信息并返回 true。 */
bool SyncEngine::update_dhcp(uint32_t now, uint32_t random_hint) noexcept {
    if (!config_.dhcp) {
        return false;
    }
    collect_used_addresses();
    const DhcpResult result = dhcp_.evaluate(dhcp_used_, random_hint);
    if (result.decision == DhcpDecision::WaitForPeers) {
        return false;  // 等对端先通告子网，本轮不提交
    }
    dhcp_.commit(result);
    if (result.decision == DhcpDecision::Unchanged) {
        return false;
    }
    my_ipv4_ = result.address;
    my_prefix_ = result.prefix;
    ET_LOGI(kTag, "dhcp assigned virtual ip=%08x/%u", static_cast<unsigned>(result.address),
            static_cast<unsigned>(result.prefix));
    // 地址进入拓扑并递增信息版本，随后重新通告。
    topology_.set_self(my_peer_id_, true, my_ipv4_, now);
    ++self_info_version_;
    if (!build_self_info(now)) {
        ET_LOGE(kTag, "cannot rebuild self route peer info after dhcp change");
    }
    return true;
}

/**
 * @brief 构造我方的同步请求。
 *
 * @details 与上游一致：请求携带**本节点已知的整个拓扑**（所有 RoutePeerInfo 与所有
 *          邻接行），而不只是自身信息。这是多跳收敛的前提——否则 A 只能学到直连的 B，
 *          永远学不到 B 另一侧的 C（经中继通信会因此失效）。
 *
 * 所有内容都有界：peer_infos 与 conn_list 分别限制在 kMaxPeerInfosBytes /
 * kMaxConnListBytes 以内，编码放不下时按条目数递减重试。
 */
size_t SyncEngine::build_request(MutBytes out, bool include_topology,
                                 uint32_t target_peer) noexcept {
    if (active_peer_count_ == 0 || out.size() < kMaxRequestBytes) {
        return 0;
    }
    SyncRequestFields fields{};
    fields.header.my_peer_id = my_peer_id_;
    // 会话 id 取目标连接自己的那一份（0 表示取第一个直连对端）——见 session_id_of 注释。
    const uint64_t target_session =
        target_peer != 0 ? session_id_of(target_peer) : active_peers_[0].session_id;
    fields.header.my_session_id = target_session != 0 ? target_session : default_session_id_;
    fields.header.is_initiator = true;

    if (include_topology) {
        if (self_peer_info_len_ == 0) {
            return 0;
        }
        // 1) peer_infos：自身优先，其次拓扑中其它已知节点（保留原始字节，未知字段不丢）。
        std::array<Bytes, kMaxRoutePeers> items{};
        size_t item_count = 0;
        items[item_count++] = Bytes(self_peer_info_.data(), self_peer_info_len_);
        for (const TopoPeer& peer : topology_.peers()) {
            if (peer.peer_id == my_peer_id_ || peer.raw_len == 0 ||
                item_count >= items.size()) {
                continue;
            }
            items[item_count++] = Bytes(peer.raw.data(), peer.raw_len);
        }
        size_t infos_len = 0;
        for (size_t n = item_count; n > 0; --n) {
            infos_len = encode_route_peer_infos(items.data(), n, peer_infos_);
            if (infos_len != 0) {
                break;
            }
        }
        if (infos_len == 0) {
            return 0;
        }
        fields.peer_infos = Bytes(peer_infos_.data(), infos_len);

        // 2) conn_list：自身行 + 拓扑中所有已知邻接行（同样有界）。
        //    使用成员缓冲，并且"放不下就减少行数"时直接改 count（不复制整张表）。
        ConnList& list = conn_list_build_;
        list = ConnList{};
        list.count = 1;
        list.rows[0].peer_id = my_peer_id_;
        list.rows[0].version = conn_row_version_;
        for (size_t i = 0; i < direct_peer_count_ && i < kMaxConnectedPerRow; ++i) {
            list.rows[0].connected[list.rows[0].connected_count++] = direct_peers_[i];
        }
        for (const TopoConnRow& row : topology_.conn_rows()) {
            if (!row.valid || row.peer_id == my_peer_id_ || list.count >= list.rows.size()) {
                continue;
            }
            ConnRow& target = list.rows[list.count];
            target.peer_id = row.peer_id;
            target.version = row.version;
            for (size_t i = 0; i < row.connected_count && i < kMaxConnectedPerRow; ++i) {
                target.connected[target.connected_count++] = row.connected[i];
            }
            ++list.count;
        }
        const size_t rows_built = list.count;
        size_t conn_len = 0;
        for (size_t n = rows_built; n > 0; --n) {
            list.count = n;
            conn_len = encode_conn_list(list, conn_list_);
            if (conn_len != 0) {
                break;
            }
        }
        list.count = rows_built;  // 还原，便于调试观察
        if (conn_len == 0) {
            return 0;
        }
        fields.conn_list = Bytes(conn_list_.data(), conn_len);
    }
    return encode_sync_request(fields, out);
}

/** @brief 加入缺失条目（去重、有界）。 */
void SyncEngine::add_missing(std::array<uint32_t, kPbMaxRepeatedItems>& list, size_t& count,
                             uint32_t peer_id) noexcept {
    if (peer_id == kInvalidPeerId || count >= list.size()) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (list[i] == peer_id) {
            return;
        }
    }
    list[count++] = peer_id;
}

/**
 * @brief 合并请求中的拓扑并收集缺失条目。
 * @details 与上游一致：邻接里引用到的未知节点、以及未知的邻接行本身，都要通过
 *          response 的 missing 列表请求补发，否则拓扑会长期缺数据。
 */
bool SyncEngine::merge_request(const SyncRequestView& request, SyncResponseView& response,
                               uint32_t now) noexcept {
    const uint32_t sender = request.header.my_peer_id;
    bool duplicate_peer_id = false;
    if (request.has_peer_infos) {
        const bool ok = for_each_route_peer_info(
            request.peer_infos, kMaxRoutePeers, [this, now, sender, &duplicate_peer_id](Bytes item) {
                RoutePeerInfoView view{};
                if (!decode_route_peer_info(item, view)) {
                    return;
                }
                // 上游 check_duplicate_peer_id：若对端上报的"它自己"的路由 id 与我们
                // 已保存的不同、且版本更旧，说明该 peer id 在别处也活着（重复 id），
                // 必须回 DuplicatePeerId 而不是把这份更旧的信息合并进来。
                if (view.peer_id == sender && view.peer_route_id != 0) {
                    const TopoPeer* known = topology_.find_peer(sender);
                    if (known != nullptr && known->peer_route_id != 0 &&
                        known->peer_route_id != view.peer_route_id &&
                        view.version < known->version) {
                        duplicate_peer_id = true;
                        return;
                    }
                }
                (void)topology_.merge_peer(view, now);
            });
        if (!ok) {
            return false;
        }
        if (duplicate_peer_id) {
            // error 字段"存在且为 0"即 DuplicatePeerId（proto3 optional 语义）。
            response.has_error = true;
            response.error_code = 0;
            ET_LOGW(kTag, "peer %u reported a duplicate peer id (route id mismatch)",
                    static_cast<unsigned>(sender));
            return true;
        }
    }
    // 邻接：list 与 bitmap 两种形式都要能解析。
    // 注意：使用成员缓冲，避免在任务栈上放 4.6 KiB 的 ConnList。
    ConnList& rows = conn_list_recv_;
    rows = ConnList{};
    if (request.has_conn_list) {
        if (!decode_conn_list(request.conn_list, rows)) {
            return false;
        }
    } else if (request.has_conn_bitmap) {
        ConnBitmap bitmap{};
        if (!decode_conn_bitmap(request.conn_bitmap, bitmap) ||
            !expand_conn_bitmap(bitmap, rows)) {
            return false;
        }
    }
    for (size_t i = 0; i < rows.count; ++i) {
        const ConnRow& row = rows.rows[i];
        if (topology_.find_peer(row.peer_id) == nullptr) {
            add_missing(response.missing_conn_peer_ids, response.missing_conn_peer_count,
                        row.peer_id);
        }
        for (size_t c = 0; c < row.connected_count; ++c) {
            if (topology_.find_peer(row.connected[c]) == nullptr) {
                add_missing(response.missing_peer_ids, response.missing_peer_count,
                            row.connected[c]);
            }
        }
        (void)topology_.merge_conn_row(row.peer_id, row.version, row.connected.data(),
                                       row.connected_count, now);
    }
    return true;
}

/** @brief 处理对端请求：合并拓扑后用本节点信息与 missing 列表应答。 */
esp_err_t SyncEngine::handle_request(Bytes request, MutBytes response, size_t& response_len,
                                     uint32_t now) noexcept {
    response_len = 0;
    SyncRequestView view{};
    if (!decode_sync_request(request, view)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (view.header.my_peer_id != kInvalidPeerId && view.header.my_peer_id == my_peer_id_) {
        ET_LOGW(kTag, "peer id conflict in sync request");
        return ESP_ERR_INVALID_ARG;
    }
    SyncResponseView out{};
    out.is_initiator = true;
    // 响应的 session_id 是本节点在**该连接**上的会话 id（每连接一份）。
    const uint64_t responder_session = session_id_of(view.header.my_peer_id);
    out.session_id = responder_session != 0 ? responder_session : default_session_id_;
    if (!merge_request(view, out, now)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t len = encode_sync_response(out, response);
    if (len == 0) {
        return ESP_FAIL;
    }
    response_len = len;
    return ESP_OK;
}

/** @brief 处理对端响应：只关心 missing 列表（响应不携带拓扑）。 */
esp_err_t SyncEngine::handle_response(Bytes response, bool& need_resend) noexcept {
    need_resend = false;
    SyncResponseView view{};
    if (!decode_sync_response(response, view)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (view.has_error) {
        ET_LOGW(kTag, "peer reported sync error %d", static_cast<int>(view.error_code));
    }
    need_resend = view.missing_peer_count != 0 || view.missing_conn_peer_count != 0;
    return ESP_OK;
}

/** @brief 清理过期拓扑条目。 */
void SyncEngine::expire_topology(uint32_t now) noexcept { topology_.expire(now); }

/** @brief 拓扑脏时重算路由。 */
bool SyncEngine::recompute_if_dirty() noexcept {
    if (!topology_.dirty()) {
        return false;
    }
    table_.recompute(topology_, my_peer_id_);
    topology_.clear_dirty();
    return true;
}

}  // namespace et
