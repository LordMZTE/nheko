#include "EventStore.h"

#include <QThread>

#include "Cache_p.h"
#include "EventAccessors.h"
#include "Logging.h"
#include "MatrixClient.h"
#include "Olm.h"

QCache<EventStore::IdIndex, mtx::events::collections::TimelineEvents> EventStore::decryptedEvents_{
  1000};
QCache<EventStore::IdIndex, mtx::events::collections::TimelineEvents> EventStore::events_by_id_{
  1000};
QCache<EventStore::Index, mtx::events::collections::TimelineEvents> EventStore::events_{1000};

EventStore::EventStore(std::string room_id, QObject *)
  : room_id_(std::move(room_id))
{
        auto range = cache::client()->getTimelineRange(room_id_);

        if (range) {
                this->first = range->first;
                this->last  = range->last;
        }

        connect(
          this,
          &EventStore::eventFetched,
          this,
          [this](std::string id,
                 std::string relatedTo,
                 mtx::events::collections::TimelineEvents timeline) {
                  cache::client()->storeEvent(room_id_, id, {timeline});

                  if (!relatedTo.empty()) {
                          auto idx = idToIndex(id);
                          if (idx)
                                  emit dataChanged(*idx, *idx);
                  }
          },
          Qt::QueuedConnection);
}

void
EventStore::handleSync(const mtx::responses::Timeline &events)
{
        if (this->thread() != QThread::currentThread())
                nhlog::db()->warn("{} called from a different thread!", __func__);

        auto range = cache::client()->getTimelineRange(room_id_);

        if (range && range->last > this->last) {
                emit beginInsertRows(toExternalIdx(this->last + 1), toExternalIdx(range->last));
                this->last = range->last;
                emit endInsertRows();
        }

        for (const auto &event : events.events) {
                std::string relates_to;
                if (auto redaction =
                      std::get_if<mtx::events::RedactionEvent<mtx::events::msg::Redaction>>(
                        &event)) {
                        relates_to = redaction->redacts;
                } else if (auto reaction =
                             std::get_if<mtx::events::RoomEvent<mtx::events::msg::Reaction>>(
                               &event)) {
                        relates_to = reaction->content.relates_to.event_id;
                } else {
                        relates_to = mtx::accessors::in_reply_to_event(event);
                }

                if (!relates_to.empty()) {
                        auto idx = cache::client()->getTimelineIndex(room_id_, relates_to);
                        if (idx)
                                emit dataChanged(toExternalIdx(*idx), toExternalIdx(*idx));
                }
        }
}

mtx::events::collections::TimelineEvents *
EventStore::event(int idx, bool decrypt)
{
        if (this->thread() != QThread::currentThread())
                nhlog::db()->warn("{} called from a different thread!", __func__);

        Index index{room_id_, toInternalIdx(idx)};
        if (index.idx > last || index.idx < first)
                return nullptr;

        auto event_ptr = events_.object(index);
        if (!event_ptr) {
                auto event_id = cache::client()->getTimelineEventId(room_id_, index.idx);
                if (!event_id)
                        return nullptr;

                auto event = cache::client()->getEvent(room_id_, *event_id);
                if (!event)
                        return nullptr;
                else
                        event_ptr =
                          new mtx::events::collections::TimelineEvents(std::move(event->data));
                events_.insert(index, event_ptr);
        }

        if (decrypt)
                if (auto encrypted =
                      std::get_if<mtx::events::EncryptedEvent<mtx::events::msg::Encrypted>>(
                        event_ptr))
                        return decryptEvent({room_id_, encrypted->event_id}, *encrypted);

        return event_ptr;
}

std::optional<int>
EventStore::idToIndex(std::string_view id) const
{
        if (this->thread() != QThread::currentThread())
                nhlog::db()->warn("{} called from a different thread!", __func__);

        auto idx = cache::client()->getTimelineIndex(room_id_, id);
        if (idx)
                return toExternalIdx(*idx);
        else
                return std::nullopt;
}
std::optional<std::string>
EventStore::indexToId(int idx) const
{
        if (this->thread() != QThread::currentThread())
                nhlog::db()->warn("{} called from a different thread!", __func__);

        return cache::client()->getTimelineEventId(room_id_, toInternalIdx(idx));
}

mtx::events::collections::TimelineEvents *
EventStore::decryptEvent(const IdIndex &idx,
                         const mtx::events::EncryptedEvent<mtx::events::msg::Encrypted> &e)
{
        if (auto cachedEvent = decryptedEvents_.object(idx))
                return cachedEvent;

        MegolmSessionIndex index;
        index.room_id    = room_id_;
        index.session_id = e.content.session_id;
        index.sender_key = e.content.sender_key;

        mtx::events::RoomEvent<mtx::events::msg::Notice> dummy;
        dummy.origin_server_ts = e.origin_server_ts;
        dummy.event_id         = e.event_id;
        dummy.sender           = e.sender;
        dummy.content.body =
          tr("-- Encrypted Event (No keys found for decryption) --",
             "Placeholder, when the message was not decrypted yet or can't be decrypted.")
            .toStdString();

        auto asCacheEntry = [&idx](mtx::events::collections::TimelineEvents &&event) {
                auto event_ptr = new mtx::events::collections::TimelineEvents(std::move(event));
                decryptedEvents_.insert(idx, event_ptr);
                return event_ptr;
        };

        try {
                if (!cache::client()->inboundMegolmSessionExists(index)) {
                        nhlog::crypto()->info("Could not find inbound megolm session ({}, {}, {})",
                                              index.room_id,
                                              index.session_id,
                                              e.sender);
                        // TODO: request megolm session_id & session_key from the sender.
                        return asCacheEntry(std::move(dummy));
                }
        } catch (const lmdb::error &e) {
                nhlog::db()->critical("failed to check megolm session's existence: {}", e.what());
                dummy.content.body = tr("-- Decryption Error (failed to communicate with DB) --",
                                        "Placeholder, when the message can't be decrypted, because "
                                        "the DB access failed when trying to lookup the session.")
                                       .toStdString();
                return asCacheEntry(std::move(dummy));
        }

        std::string msg_str;
        try {
                auto session = cache::client()->getInboundMegolmSession(index);
                auto res     = olm::client()->decrypt_group_message(session, e.content.ciphertext);
                msg_str      = std::string((char *)res.data.data(), res.data.size());
        } catch (const lmdb::error &e) {
                nhlog::db()->critical("failed to retrieve megolm session with index ({}, {}, {})",
                                      index.room_id,
                                      index.session_id,
                                      index.sender_key,
                                      e.what());
                dummy.content.body =
                  tr("-- Decryption Error (failed to retrieve megolm keys from db) --",
                     "Placeholder, when the message can't be decrypted, because the DB access "
                     "failed.")
                    .toStdString();
                return asCacheEntry(std::move(dummy));
        } catch (const mtx::crypto::olm_exception &e) {
                nhlog::crypto()->critical("failed to decrypt message with index ({}, {}, {}): {}",
                                          index.room_id,
                                          index.session_id,
                                          index.sender_key,
                                          e.what());
                dummy.content.body =
                  tr("-- Decryption Error (%1) --",
                     "Placeholder, when the message can't be decrypted. In this case, the Olm "
                     "decrytion returned an error, which is passed as %1.")
                    .arg(e.what())
                    .toStdString();
                return asCacheEntry(std::move(dummy));
        }

        // Add missing fields for the event.
        json body                = json::parse(msg_str);
        body["event_id"]         = e.event_id;
        body["sender"]           = e.sender;
        body["origin_server_ts"] = e.origin_server_ts;
        body["unsigned"]         = e.unsigned_data;

        // relations are unencrypted in content...
        if (json old_ev = e; old_ev["content"].count("m.relates_to") != 0)
                body["content"]["m.relates_to"] = old_ev["content"]["m.relates_to"];

        json event_array = json::array();
        event_array.push_back(body);

        std::vector<mtx::events::collections::TimelineEvents> temp_events;
        mtx::responses::utils::parse_timeline_events(event_array, temp_events);

        if (temp_events.size() == 1) {
                auto encInfo = mtx::accessors::file(temp_events[0]);

                if (encInfo)
                        emit newEncryptedImage(encInfo.value());

                return asCacheEntry(std::move(temp_events[0]));
        }

        dummy.content.body =
          tr("-- Encrypted Event (Unknown event type) --",
             "Placeholder, when the message was decrypted, but we couldn't parse it, because "
             "Nheko/mtxclient don't support that event type yet.")
            .toStdString();
        return asCacheEntry(std::move(dummy));
}

mtx::events::collections::TimelineEvents *
EventStore::event(std::string_view id, std::string_view related_to, bool decrypt)
{
        if (this->thread() != QThread::currentThread())
                nhlog::db()->warn("{} called from a different thread!", __func__);

        if (id.empty())
                return nullptr;

        IdIndex index{room_id_, std::string(id.data(), id.size())};

        auto event_ptr = events_by_id_.object(index);
        if (!event_ptr) {
                auto event = cache::client()->getEvent(room_id_, index.id);
                if (!event) {
                        http::client()->get_event(
                          room_id_,
                          index.id,
                          [this,
                           relatedTo = std::string(related_to.data(), related_to.size()),
                           id = index.id](const mtx::events::collections::TimelineEvents &timeline,
                                          mtx::http::RequestErr err) {
                                  if (err) {
                                          nhlog::net()->error(
                                            "Failed to retrieve event with id {}, which was "
                                            "requested to show the replyTo for event {}",
                                            relatedTo,
                                            id);
                                          return;
                                  }
                                  emit eventFetched(id, relatedTo, timeline);
                          });
                        return nullptr;
                }
                event_ptr = new mtx::events::collections::TimelineEvents(std::move(event->data));
                events_by_id_.insert(index, event_ptr);
        }

        if (decrypt)
                if (auto encrypted =
                      std::get_if<mtx::events::EncryptedEvent<mtx::events::msg::Encrypted>>(
                        event_ptr))
                        return decryptEvent(index, *encrypted);

        return event_ptr;
}
