

#include "cgimap/backend/apidb/changeset_upload/changeset_updater.hpp"
#include "cgimap/backend/apidb/pqxx_string_traits.hpp"
#include "cgimap/http.hpp"
#include "cgimap/logger.hpp"

#include <boost/format.hpp>
#include <pqxx/pqxx>
#include <stdexcept>

using boost::format;

ApiDB_Changeset_Updater::ApiDB_Changeset_Updater(Transaction_Manager &_m,
                                                 osm_changeset_id_t _changeset,
                                                 osm_user_id_t _uid)
    : m(_m), num_changes(0), changeset(_changeset), uid(_uid) {}

ApiDB_Changeset_Updater::~ApiDB_Changeset_Updater() {}

void ApiDB_Changeset_Updater::lock_current_changeset() {

  m.prepare("changeset_current_lock",
            R"( SELECT id, 
                       user_id,
                       created_at,
                       num_changes, 
                       to_char(closed_at,'YYYY-MM-DD HH24:MM:SS "UTC"') as closed_at, 
                       ((now() at time zone 'utc') > closed_at) as is_closed,
                       to_char((now() at time zone 'utc'),'YYYY-MM-DD HH24:MM:SS "UTC"') as current_time,
                FROM changesets WHERE id = $1 AND user_id = $2 
                FOR UPDATE 
             )");

  pqxx::result r = m.prepared("changeset_current_lock")(changeset)(uid).exec();

  if (r.affected_rows() != 1)
    throw http::conflict("The user doesn't own that changeset");

  if (r[0]["is_closed"].as<bool>())
    throw http::conflict((boost::format("The changeset %1% was closed at %2%") %
                          changeset % r[0]["closed_at"].as<std::string>())
                             .str());
  if (r[0]["num_changes"].as<int>() > CHANGESET_MAX_ELEMENTS)
    throw http::conflict((boost::format("The changeset %1% was closed at %2%") %
                          changeset % r[0]["current_time"].as<std::string>())
                             .str());

  num_changes = r[0]["num_changes"].as<int>();
}

void ApiDB_Changeset_Updater::update_changeset(long num_new_changes,
                                               bbox_t bbox) {

  if (num_changes + num_new_changes > CHANGESET_MAX_ELEMENTS)
    throw http::conflict((boost::format("The changeset %1% was closed at %2%") %
                          changeset % "now")
                             .str()); // TODO: proper timestamp instead of now

  num_changes += num_new_changes;

  bbox_t initial;

  bool valid_bbox = !(bbox == initial);

  /*
   update for closed_at according to logic in update_closed_at in
   changeset.rb:
   set the auto-close time to be one hour in the future unless
   that would make it more than 24h long, in which case clip to
   24h, as this has been decided is a reasonable time limit.

   */

  m.prepare("changeset_update",
            R"( 
       UPDATE changesets 
       SET num_changes = ($1 :: integer),
           min_lat = $2,
           min_lon = $3,
           max_lat = $4,
           max_lon = $5,
           closed_at = 
             CASE
                WHEN (closed_at - created_at) > 
                     (($6 ::interval) - ($7 ::interval)) THEN
                  created_at + ($7 ::interval)
                ELSE 
                  now() at time zone 'utc' + ($7 ::interval)
             END
       WHERE id = $8

       )");

  if (valid_bbox) {
    pqxx::result r =
        m.prepared("changeset_update")(num_changes)(bbox.minlat)(bbox.minlon)(
             bbox.maxlat)(bbox.maxlon)(MAX_TIME_OPEN)(IDLE_TIMEOUT)(changeset)
            .exec();

    if (r.affected_rows() != 1)
      throw http::server_error("Cannot update changeset");
  } else {
    pqxx::result r = m.prepared("changeset_update")(num_changes)()()()()(
                          MAX_TIME_OPEN)(IDLE_TIMEOUT)(changeset)
                         .exec();

    if (r.affected_rows() != 1)
      throw http::server_error("Cannot update changeset");
  }
}
