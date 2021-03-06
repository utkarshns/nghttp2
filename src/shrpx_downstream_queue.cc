/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx_downstream_queue.h"

#include <cassert>
#include <limits>

#include "shrpx_downstream.h"

namespace shrpx {

DownstreamQueue::HostEntry::HostEntry() : num_active(0) {}

DownstreamQueue::DownstreamQueue(size_t conn_max_per_host, bool unified_host)
    : conn_max_per_host_(conn_max_per_host == 0
                             ? std::numeric_limits<size_t>::max()
                             : conn_max_per_host),
      unified_host_(unified_host) {}

DownstreamQueue::~DownstreamQueue() {
  dlist_delete_all(downstreams_);
  for (auto &p : host_entries_) {
    auto &ent = p.second;
    dlist_delete_all(ent.blocked);
  }
}

void DownstreamQueue::add_pending(std::unique_ptr<Downstream> downstream) {
  downstream->set_dispatch_state(Downstream::DISPATCH_PENDING);
  downstreams_.append(downstream.release());
}

void DownstreamQueue::mark_failure(Downstream *downstream) {
  downstream->set_dispatch_state(Downstream::DISPATCH_FAILURE);
}

DownstreamQueue::HostEntry &
DownstreamQueue::find_host_entry(const std::string &host) {
  auto itr = host_entries_.find(host);
  if (itr == std::end(host_entries_)) {
#ifdef HAVE_STD_MAP_EMPLACE
    std::tie(itr, std::ignore) = host_entries_.emplace(host, HostEntry());
#else  // !HAVE_STD_MAP_EMPLACE
    // for g++-4.7
    std::tie(itr, std::ignore) = host_entries_.insert({host, HostEntry()});
#endif // !HAVE_STD_MAP_EMPLACE
  }
  return (*itr).second;
}

const std::string &
DownstreamQueue::make_host_key(const std::string &host) const {
  static std::string empty_key;
  return unified_host_ ? empty_key : host;
}

const std::string &
DownstreamQueue::make_host_key(Downstream *downstream) const {
  return make_host_key(downstream->get_request_http2_authority());
}

void DownstreamQueue::mark_active(Downstream *downstream) {
  auto &ent = find_host_entry(make_host_key(downstream));
  ++ent.num_active;

  downstream->set_dispatch_state(Downstream::DISPATCH_ACTIVE);
}

void DownstreamQueue::mark_blocked(Downstream *downstream) {
  auto &ent = find_host_entry(make_host_key(downstream));

  downstream->set_dispatch_state(Downstream::DISPATCH_BLOCKED);

  auto link = new BlockedLink{};
  downstream->attach_blocked_link(link);
  ent.blocked.append(link);
}

bool DownstreamQueue::can_activate(const std::string &host) const {
  auto itr = host_entries_.find(make_host_key(host));
  if (itr == std::end(host_entries_)) {
    return true;
  }
  auto &ent = (*itr).second;
  return ent.num_active < conn_max_per_host_;
}

namespace {
bool remove_host_entry_if_empty(const DownstreamQueue::HostEntry &ent,
                                DownstreamQueue::HostEntryMap &host_entries,
                                const std::string &host) {
  if (ent.blocked.empty() && ent.num_active == 0) {
    host_entries.erase(host);
    return true;
  }
  return false;
}
} // namespace

Downstream *DownstreamQueue::remove_and_get_blocked(Downstream *downstream) {
  // Delete downstream when this function returns.
  auto delptr = std::unique_ptr<Downstream>(downstream);

  if (downstream->get_dispatch_state() != Downstream::DISPATCH_ACTIVE) {
    assert(downstream->get_dispatch_state() != Downstream::DISPATCH_NONE);
    downstreams_.remove(downstream);
    return nullptr;
  }

  downstreams_.remove(downstream);

  auto &host = make_host_key(downstream);
  auto &ent = find_host_entry(host);
  --ent.num_active;

  if (remove_host_entry_if_empty(ent, host_entries_, host)) {
    return nullptr;
  }

  if (ent.num_active >= conn_max_per_host_) {
    return nullptr;
  }

  for (auto link = ent.blocked.head; link;) {
    auto next = link->dlnext;
    if (!link->downstream) {
      ent.blocked.remove(link);
      link = next;
      continue;
    }
    auto next_downstream = link->downstream;
    next_downstream->detach_blocked_link(link);
    ent.blocked.remove(link);
    delete link;
    remove_host_entry_if_empty(ent, host_entries_, host);
    return next_downstream;
  }
  return nullptr;
}

Downstream *DownstreamQueue::get_downstreams() const {
  return downstreams_.head;
}

} // namespace shrpx
