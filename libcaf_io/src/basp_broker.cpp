/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright 2011-2018 Dominik Charousset                                     *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/io/basp_broker.hpp"

#include <chrono>
#include <limits>

#include "caf/actor_registry.hpp"
#include "caf/actor_system_config.hpp"
#include "caf/after.hpp"
#include "caf/defaults.hpp"
#include "caf/detail/sync_request_bouncer.hpp"
#include "caf/event_based_actor.hpp"
#include "caf/forwarding_actor_proxy.hpp"
#include "caf/io/basp/all.hpp"
#include "caf/io/connection_helper.hpp"
#include "caf/io/middleman.hpp"
#include "caf/io/network/interfaces.hpp"
#include "caf/make_counted.hpp"
#include "caf/sec.hpp"
#include "caf/send.hpp"

namespace caf {
namespace io {

const char* basp_broker_state::name = "basp_broker";

/******************************************************************************
 *                             basp_broker_state                              *
 ******************************************************************************/

basp_broker_state::basp_broker_state(broker* selfptr)
  : basp::instance::callee(selfptr->system(),
                           static_cast<proxy_registry::backend&>(*this)),
    self(selfptr),
    instance(selfptr, *this) {
  CAF_ASSERT(this_node() != none);
}

basp_broker_state::~basp_broker_state() {
  // make sure all spawn servers are down
  for (auto& kvp : spawn_servers)
    anon_send_exit(kvp.second, exit_reason::kill);
}

strong_actor_ptr basp_broker_state::make_proxy(node_id nid, actor_id aid) {
  CAF_LOG_TRACE(CAF_ARG(nid) << CAF_ARG(aid));
  CAF_ASSERT(nid != this_node());
  if (nid == none || aid == invalid_actor_id)
    return nullptr;
  // this member function is being called whenever we deserialize a
  // payload received from a remote node; if a remote node A sends
  // us a handle to a third node B, then we assume that A offers a route to B
  if (nid != this_context->id
      && instance.tbl().add_indirect(this_context->id, nid))
    learned_new_node_indirectly(nid);
  // we need to tell remote side we are watching this actor now;
  // use a direct route if possible, i.e., when talking to a third node
  // create proxy and add functor that will be called if we
  // receive a basp::down_message
  auto mm = &system().middleman();
  actor_config cfg;
  auto res = make_actor<forwarding_actor_proxy, strong_actor_ptr>(
    aid, nid, &(self->home_system()), cfg, self);
  strong_actor_ptr selfptr{self->ctrl()};
  res->get()->attach_functor([=](const error& rsn) {
    mm->backend().post([=] {
      // using res->id() instead of aid keeps this actor instance alive
      // until the original instance terminates, thus preventing subtle
      // bugs with attachables
      auto bptr = static_cast<basp_broker*>(selfptr->get());
      if (!bptr->getf(abstract_actor::is_terminated_flag))
        bptr->state.proxies().erase(nid, res->id(), rsn);
    });
  });
  return res;
}

void basp_broker_state::finalize_handshake(const node_id& nid, actor_id aid,
                                           std::set<std::string>& sigs) {
  CAF_LOG_TRACE(CAF_ARG(nid) << CAF_ARG(aid) << CAF_ARG(sigs));
  CAF_ASSERT(this_context != nullptr);
  this_context->id = nid;
  auto& cb = this_context->callback;
  if (cb == none)
    return;
  strong_actor_ptr ptr;
  // aid can be invalid when connecting to the default port of a node
  if (aid != invalid_actor_id) {
    if (nid == this_node()) {
      // connected to self
      ptr = actor_cast<strong_actor_ptr>(system().registry().get(aid));
      CAF_LOG_DEBUG_IF(!ptr, "actor not found:" << CAF_ARG(aid));
    } else {
      ptr = namespace_.get_or_put(nid, aid);
      CAF_LOG_ERROR_IF(!ptr, "creating actor in finalize_handshake failed");
    }
  }
  cb->deliver(nid, std::move(ptr), std::move(sigs));
  cb = none;
}

void basp_broker_state::purge_state(const node_id& nid) {
  CAF_LOG_TRACE(CAF_ARG(nid));
  // Destroy all proxies of the lost node.
  namespace_.erase(nid);
  // Cleanup all remaining references to the lost node.
  for (auto& kvp : monitored_actors)
    kvp.second.erase(nid);
}

void basp_broker_state::send_basp_down_message(const node_id& nid, actor_id aid,
                                               error rsn) {
  CAF_LOG_TRACE(CAF_ARG(nid) << CAF_ARG(aid) << CAF_ARG(rsn));
  auto path = instance.tbl().lookup(nid);
  if (!path) {
    CAF_LOG_INFO("cannot send exit message for proxy, no route to host:"
                 << CAF_ARG(nid));
    return;
  }
  instance.write_down_message(self->context(), get_buffer(path->hdl), nid, aid,
                              rsn);
  instance.flush(*path);
}

void basp_broker_state::proxy_announced(const node_id& nid, actor_id aid) {
  CAF_LOG_TRACE(CAF_ARG(nid) << CAF_ARG(aid));
  // source node has created a proxy for one of our actors
  auto ptr = system().registry().get(aid);
  if (ptr == nullptr) {
    CAF_LOG_DEBUG("kill proxy immediately");
    // kill immediately if actor has already terminated
    send_basp_down_message(nid, aid, exit_reason::unknown);
  } else {
    auto entry = ptr->address();
    auto i = monitored_actors.find(entry);
    if (i == monitored_actors.end()) {
      self->monitor(ptr);
      std::unordered_set<node_id> tmp{nid};
      monitored_actors.emplace(entry, std::move(tmp));
    } else {
      i->second.emplace(nid);
    }
  }
}

void basp_broker_state::handle_down_msg(down_msg& dm) {
  auto i = monitored_actors.find(dm.source);
  if (i == monitored_actors.end())
    return;
  for (auto& nid : i->second)
    send_basp_down_message(nid, dm.source.id(), dm.reason);
  monitored_actors.erase(i);
}

void basp_broker_state::learned_new_node(const node_id& nid) {
  CAF_LOG_TRACE(CAF_ARG(nid));
  if (spawn_servers.count(nid) > 0) {
    CAF_LOG_ERROR("learned_new_node called for known node " << CAF_ARG(nid));
    return;
  }
  auto tmp = system().spawn<hidden>([=](event_based_actor* tself) -> behavior {
    CAF_LOG_TRACE("");
    // terminate when receiving a down message
    tself->set_down_handler([=](down_msg& dm) {
      CAF_LOG_TRACE(CAF_ARG(dm));
      tself->quit(std::move(dm.reason));
    });
    // skip messages until we receive the initial ok_atom
    tself->set_default_handler(skip);
    return {
      [=](ok_atom, const std::string& /* key == "info" */,
          const strong_actor_ptr& config_serv, const std::string& /* name */) {
        CAF_LOG_TRACE(CAF_ARG(config_serv));
        // drop unexpected messages from this point on
        tself->set_default_handler(print_and_drop);
        if (!config_serv)
          return;
        tself->monitor(config_serv);
        tself->become(
          [=](spawn_atom, std::string& type, message& args)
          -> delegated<strong_actor_ptr, std::set<std::string>> {
            CAF_LOG_TRACE(CAF_ARG(type) << CAF_ARG(args));
            tself->delegate(actor_cast<actor>(std::move(config_serv)),
                            get_atom::value, std::move(type),
                            std::move(args));
            return {};
          }
        );
      },
      after(std::chrono::minutes(5)) >> [=] {
        CAF_LOG_INFO("no spawn server found:" << CAF_ARG(nid));
        tself->quit();
      }
    };
  });
  spawn_servers.emplace(nid, tmp);
  using namespace detail;
  auto tmp_ptr = actor_cast<strong_actor_ptr>(tmp);
  system().registry().put(tmp.id(), tmp_ptr);
  std::vector<strong_actor_ptr> stages;
  if (!instance.dispatch(self->context(), tmp_ptr, stages, nid,
                         static_cast<uint64_t>(atom("SpawnServ")),
                         basp::header::named_receiver_flag, make_message_id(),
                         make_message(sys_atom::value, get_atom::value,
                                      "info"))) {
    CAF_LOG_ERROR("learned_new_node called, but no route to remote node"
                  << CAF_ARG(nid));
  }
}

void basp_broker_state::learned_new_node_directly(const node_id& nid,
                                                  bool was_indirectly_before) {
  CAF_LOG_TRACE(CAF_ARG(nid));
  if (!was_indirectly_before)
    learned_new_node(nid);
}

void basp_broker_state::learned_new_node_indirectly(const node_id& nid) {
  CAF_LOG_TRACE(CAF_ARG(nid));
  learned_new_node(nid);
  if (!automatic_connections)
    return;
  // This member function gets only called once, after adding a new indirect
  // connection to the routing table; hence, spawning our helper here exactly
  // once and there is no need to track in-flight connection requests.
  using namespace detail;
  auto tmp = get_or(config(), "middleman.attach-utility-actors", false)
               ? system().spawn<hidden>(connection_helper, self)
               : system().spawn<detached + hidden>(connection_helper, self);
  auto sender = actor_cast<strong_actor_ptr>(tmp);
  system().registry().put(sender->id(), sender);
  std::vector<strong_actor_ptr> fwd_stack;
  if (!instance.dispatch(self->context(), sender, fwd_stack, nid,
                         static_cast<uint64_t>(atom("ConfigServ")),
                         basp::header::named_receiver_flag, make_message_id(),
                         make_message(get_atom::value,
                                      "basp.default-connectivity-tcp"))) {
    CAF_LOG_ERROR("learned_new_node_indirectly called, but no route to nid");
  }
}

void basp_broker_state::set_context(connection_handle hdl) {
  CAF_LOG_TRACE(CAF_ARG(hdl));
  auto i = ctx.find(hdl);
  if (i == ctx.end()) {
    CAF_LOG_DEBUG("create new BASP context:" << CAF_ARG(hdl));
    basp::header hdr{basp::message_type::server_handshake, 0, 0, 0,
                     invalid_actor_id, invalid_actor_id};
    i = ctx
          .emplace(hdl, basp::endpoint_context{basp::await_header, hdr, hdl,
                                               none, 0, 0, none})
          .first;
  }
  this_context = &i->second;
}

void basp_broker_state::cleanup(connection_handle hdl) {
  CAF_LOG_TRACE(CAF_ARG(hdl));
  // Remove handle from the routing table and clean up any node-specific state
  // we might still have.
  if (auto nid = instance.tbl().erase_direct(hdl))
    purge_state(nid);
  // Remove the context for `hdl`, making sure clients receive an error in case
  // this connection was closed during handshake.
  auto i = ctx.find(hdl);
  if (i != ctx.end()) {
    auto& ref = i->second;
    CAF_ASSERT(i->first == ref.hdl);
    if (ref.callback) {
      CAF_LOG_DEBUG("connection closed during handshake");
      ref.callback->deliver(sec::disconnect_during_handshake);
    }
    ctx.erase(i);
  }
}

basp_broker_state::buffer_type&
basp_broker_state::get_buffer(connection_handle hdl) {
  return self->wr_buf(hdl);
}

void basp_broker_state::flush(connection_handle hdl) {
  self->flush(hdl);
}

void basp_broker_state::handle_heartbeat() {
  // nop
}

execution_unit* basp_broker_state::current_execution_unit() {
  return self->context();
}

/******************************************************************************
 *                                basp_broker                                 *
 ******************************************************************************/

basp_broker::basp_broker(actor_config& cfg)
    : stateful_actor<basp_broker_state, broker>(cfg) {
  set_down_handler([](local_actor* ptr, down_msg& x) {
    static_cast<basp_broker*>(ptr)->state.handle_down_msg(x);
  });
}

behavior basp_broker::make_behavior() {
  CAF_LOG_TRACE(CAF_ARG(system().node()));
  if (get_or(config(), "middleman.enable-automatic-connections", false)) {
    CAF_LOG_DEBUG("enable automatic connections");
    // open a random port and store a record for our peers how to
    // connect to this broker directly in the configuration server
    auto res = add_tcp_doorman(uint16_t{0});
    if (res) {
      auto port = res->second;
      auto addrs = network::interfaces::list_addresses(false);
      auto config_server = system().registry().get(atom("ConfigServ"));
      send(actor_cast<actor>(config_server), put_atom::value,
           "basp.default-connectivity-tcp",
           make_message(port, std::move(addrs)));
    }
    state.automatic_connections = true;
  }
  auto heartbeat_interval = get_or(config(), "middleman.heartbeat-interval",
                                   defaults::middleman::heartbeat_interval);
  if (heartbeat_interval > 0) {
    CAF_LOG_DEBUG("enable heartbeat" << CAF_ARG(heartbeat_interval));
    send(this, tick_atom::value, heartbeat_interval);
  }
  return {
    // received from underlying broker implementation
    [=](new_data_msg& msg) {
      CAF_LOG_TRACE(CAF_ARG(msg.handle));
      state.set_context(msg.handle);
      auto& ctx = *state.this_context;
      auto next = state.instance.handle(context(), msg, ctx.hdr,
                                        ctx.cstate == basp::await_payload);
      if (next == basp::close_connection) {
        state.cleanup(msg.handle);
        close(msg.handle);
        return;
      }
      if (next != ctx.cstate) {
        auto rd_size = next == basp::await_payload
                       ? ctx.hdr.payload_len
                       : basp::header_size;
        configure_read(msg.handle, receive_policy::exactly(rd_size));
        ctx.cstate = next;
      }
    },
    // received from proxy instances
    [=](forward_atom, strong_actor_ptr& src,
        const std::vector<strong_actor_ptr>& fwd_stack,
        strong_actor_ptr& dest, message_id mid, const message& msg) {
      CAF_LOG_TRACE(CAF_ARG(src) << CAF_ARG(dest)
                    << CAF_ARG(mid) << CAF_ARG(msg));
      if (!dest || system().node() == dest->node()) {
        CAF_LOG_WARNING("cannot forward to invalid or local actor:"
                        << CAF_ARG(dest));
        return;
      }
      if (src && system().node() == src->node())
        system().registry().put(src->id(), src);
      if (!state.instance.dispatch(context(), src, fwd_stack, dest->node(),
                                   dest->id(), 0, mid, msg)
          && mid.is_request()) {
        detail::sync_request_bouncer srb{exit_reason::remote_link_unreachable};
        srb(src, mid);
      }
    },
    // received from some system calls like whereis
    [=](forward_atom, const node_id& dest_node, atom_value dest_name,
        const message& msg) -> result<message> {
      auto cme = current_mailbox_element();
      if (cme == nullptr || cme->sender == nullptr)
        return sec::invalid_argument;
      CAF_LOG_TRACE(CAF_ARG2("sender", cme->sender)
                    << ", " << CAF_ARG(dest_node)
                    << ", " << CAF_ARG(dest_name)
                    << ", " << CAF_ARG(msg));
      auto& sender = cme->sender;
      if (system().node() == sender->node())
        system().registry().put(sender->id(), sender);
      if (!state.instance.dispatch(context(), sender, cme->stages,
                                   dest_node, static_cast<uint64_t>(dest_name),
                                   basp::header::named_receiver_flag, cme->mid,
                                   msg)) {
        detail::sync_request_bouncer srb{exit_reason::remote_link_unreachable};
        srb(sender, cme->mid);
      }
      return delegated<message>();
    },
    // received from proxy instances to signal that we need to send a BASP
    // monitor_message to the origin node
    [=](monitor_atom, const strong_actor_ptr& proxy) {
      if (proxy == nullptr) {
        CAF_LOG_WARNING("received a monitor message from an invalid proxy");
        return;
      }
      auto route = state.instance.tbl().lookup(proxy->node());
      if (route == none) {
        CAF_LOG_DEBUG("connection to origin already lost, kill proxy");
        state.instance.proxies().erase(proxy->node(), proxy->id());
        return;
      }
      CAF_LOG_DEBUG("write monitor_message:" << CAF_ARG(proxy));
      // tell remote side we are monitoring this actor now
      auto hdl = route->hdl;
      state.instance.write_monitor_message(context(), state.get_buffer(hdl),
                                           proxy->node(), proxy->id());
      flush(hdl);
    },
    // received from underlying broker implementation
    [=](const new_connection_msg& msg) {
      CAF_LOG_TRACE(CAF_ARG(msg.handle));
      auto& bi = state.instance;
      bi.write_server_handshake(context(), state.get_buffer(msg.handle),
                                local_port(msg.source));
      state.flush(msg.handle);
      configure_read(msg.handle, receive_policy::exactly(basp::header_size));
    },
    // received from underlying broker implementation
    [=](const connection_closed_msg& msg) {
      CAF_LOG_TRACE(CAF_ARG(msg.handle));
      state.cleanup(msg.handle);
    },
    // received from underlying broker implementation
    [=](const acceptor_closed_msg& msg) {
      CAF_LOG_TRACE("");
      auto port = local_port(msg.handle);
      state.instance.remove_published_actor(port);
    },
    // received from middleman actor
    [=](publish_atom, doorman_ptr& ptr, uint16_t port,
        const strong_actor_ptr& whom, std::set<std::string>& sigs) {
      CAF_LOG_TRACE(CAF_ARG(ptr) << CAF_ARG(port)
                    << CAF_ARG(whom) << CAF_ARG(sigs));
      CAF_ASSERT(ptr != nullptr);
      add_doorman(std::move(ptr));
      if (whom)
        system().registry().put(whom->id(), whom);
      state.instance.add_published_actor(port, whom, std::move(sigs));
    },
    // received from middleman actor (delegated)
    [=](connect_atom, scribe_ptr& ptr, uint16_t port) {
      CAF_LOG_TRACE(CAF_ARG(ptr) << CAF_ARG(port));
      CAF_ASSERT(ptr != nullptr);
      auto rp = make_response_promise();
      auto hdl = ptr->hdl();
      add_scribe(std::move(ptr));
      auto& ctx = state.ctx[hdl];
      ctx.hdl = hdl;
      ctx.remote_port = port;
      ctx.cstate = basp::await_header;
      ctx.callback = rp;
      // await server handshake
      configure_read(hdl, receive_policy::exactly(basp::header_size));
    },
    [=](delete_atom, const node_id& nid, actor_id aid) {
      CAF_LOG_TRACE(CAF_ARG(nid) << ", " << CAF_ARG(aid));
      state.proxies().erase(nid, aid);
    },
    [=](unpublish_atom, const actor_addr& whom, uint16_t port) -> result<void> {
      CAF_LOG_TRACE(CAF_ARG(whom) << CAF_ARG(port));
      auto cb = make_callback(
        [&](const strong_actor_ptr&, uint16_t x) -> error {
          close(hdl_by_port(x));
          return none;
        }
      );
      if (state.instance.remove_published_actor(whom, port, &cb) == 0)
        return sec::no_actor_published_at_port;
      return unit;
    },
    [=](close_atom, uint16_t port) -> result<void> {
      if (port == 0)
        return sec::cannot_close_invalid_port;
      // it is well-defined behavior to not have an actor published here,
      // hence the result can be ignored safely
      state.instance.remove_published_actor(port, nullptr);
      auto res = close(hdl_by_port(port));
      if (res)
        return unit;
      return sec::cannot_close_invalid_port;
    },
    [=](get_atom, const node_id& x)
    -> std::tuple<node_id, std::string, uint16_t> {
      std::string addr;
      uint16_t port = 0;
      auto hdl = state.instance.tbl().lookup_direct(x);
      if (hdl) {
        addr = remote_addr(*hdl);
        port = remote_port(*hdl);
      }
      return std::make_tuple(x, std::move(addr), port);
    },
    [=](tick_atom, size_t interval) {
      state.instance.handle_heartbeat(context());
      delayed_send(this, std::chrono::milliseconds{interval},
                   tick_atom::value, interval);
    }
  };
}

resumable::resume_result basp_broker::resume(execution_unit* ctx, size_t mt) {
  ctx->proxy_registry_ptr(&state.instance.proxies());
  auto guard = detail::make_scope_guard([=] {
    ctx->proxy_registry_ptr(nullptr);
  });
  return super::resume(ctx, mt);
}

proxy_registry* basp_broker::proxy_registry_ptr() {
  return &state.instance.proxies();
}

} // namespace io
} // namespace caf
